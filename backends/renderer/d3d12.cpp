// D3D12 renderer: records into host-owned command lists with an explicit
// frame-slot completion contract. Encoded-SDR baseline, premultiplied
// ONE/INV_SRC_ALPHA blending, no sRGB conversion. No queues, fences, waits,
// windows or threads are created here.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wui/backends/d3d12.hpp>

#include "d3d12_descriptors.hpp"
#include "d3d12_upload.hpp"
#include "scissor.hpp"
#include "wui_d3d12_shader_source.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace wui::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

// Resource binding tier 1 limit for a shader-visible CBV/SRV/UAV heap.
constexpr std::uint32_t kShaderVisibleHeapLimit = 1'000'000;
constexpr std::uint64_t kInitialUploadBytes = 256 * 1024;

bool removed(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

Error hr_error(HRESULT hr, const char* what) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), " (hr=0x%08lX)", static_cast<unsigned long>(hr));
    return Error{removed(hr) ? ErrorCode::device_lost : ErrorCode::backend_failure,
        std::string(what) + buffer};
}

Error lost_error() { return Error{ErrorCode::device_lost, "D3D12 device is removed"}; }

std::uint64_t fence_completed_value(ID3D12Fence* fence) { return fence->GetCompletedValue(); }

// Snapshot identity plus extent and alpha: equal handles and revisions from
// different TextureStores must not share a native texture.
detail::TextureKey texture_key(const TextureSnapshot& texture) noexcept {
    return {texture.handle.slot, texture.handle.generation, texture.revision,
        texture.width, texture.height, static_cast<std::uint32_t>(texture.alpha), &texture};
}

struct ViewportConstants {
    float origin_x = 0, origin_y = 0, size_w = 1, size_h = 1;
};
struct ModeConstants {
    std::uint32_t straight = 1;
    std::uint32_t pad[3] = {};
};
static_assert(sizeof(ViewportConstants) == 16);
static_assert(sizeof(ModeConstants) == 16);

enum RootParameter : UINT { kTextureTable = 0, kViewportConstants = 1, kModeConstants = 2 };

Result<ComPtr<ID3DBlob>> compile_shader(const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(detail::kShaderSource, sizeof(detail::kShaderSource) - 1,
        "wui_d3d12", nullptr, nullptr, entry, target, flags, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        std::string message("D3D12 shader compilation failed for ");
        message += entry;
        if (errors && errors->GetBufferPointer() != nullptr) {
            message += ": ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        return Error{ErrorCode::backend_failure, std::move(message)};
    }
    return code;
}

D3D12_RESOURCE_DESC buffer_desc(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

} // namespace

struct Renderer::Impl {
    struct Slot {
        ComPtr<ID3D12Resource> upload; // persistently mapped upload heap buffer
        std::byte* mapped = nullptr;
        detail::UploadRing ring;
        D3D12_VERTEX_BUFFER_VIEW vertices{};
        D3D12_INDEX_BUFFER_VIEW indices{};
        DrawPacket packet; // lease held until the slot retires or is abandoned
        ComPtr<ID3D12Fence> fence;
    };
    struct Native {
        ComPtr<ID3D12Resource> texture;
        std::weak_ptr<const TextureSnapshot> source;
    };
    struct Copy {
        std::uint32_t entry = 0;
        std::uint64_t offset = 0;
        detail::Footprint footprint;
        std::uint32_t width = 0, height = 0;
    };

    Config config;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptor_size = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu{};
    detail::DescriptorRanges descriptors;
    detail::FrameTracker tracker;
    std::vector<Slot> slots;
    std::vector<Native> natives; // indexed by tracker entry
    // Reused per-frame scratch; capacity persists across frames.
    std::vector<Copy> copies;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::vector<std::uint32_t> retired;
    detail::CompletedValueFn completed_value = &fence_completed_value;
    bool closed = false;
    bool lost = false;

    void lose() noexcept {
        lost = true;
        tracker.lose();
    }
    void release_slot(std::uint32_t index) noexcept {
        slots[index].packet.reset();
        slots[index].fence.Reset();
    }
    // Marks entries whose CPU snapshot died: they never match a key again,
    // even if a new snapshot reuses the address.
    void expire_dead() noexcept {
        for (std::uint32_t i = 0; i < natives.size(); ++i)
            if (tracker.live(i) && natives[i].source.expired()) tracker.expire(i);
    }
    void retire_textures() {
        expire_dead();
        retired.clear();
        tracker.retire_unreferenced(retired);
        for (const auto entry : retired) natives[entry] = Native{};
    }
    // Polls one submitted slot; frees it when its fence reached the value.
    Status poll(std::uint32_t index) {
        const auto completed = completed_value(slots[index].fence.Get());
        const auto status = tracker.retire(index, completed);
        if (!status) {
            release_slot(index);
            return std::nullopt;
        }
        if (status->code == ErrorCode::device_lost) lose();
        return status;
    }
};

PreparedFrame::PreparedFrame(Renderer* owner, std::uint32_t index) noexcept
    : owner_(owner), index_(index) {}

PreparedFrame::PreparedFrame(PreparedFrame&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {}

PreparedFrame& PreparedFrame::operator=(PreparedFrame&& other) noexcept {
    if (this != &other) {
        if (owner_) owner_->abandon(std::move(*this));
        owner_ = std::exchange(other.owner_, nullptr);
        index_ = other.index_;
    }
    return *this;
}

PreparedFrame::~PreparedFrame() {
    // The host must have discarded an unsubmitted command list before this point.
    if (owner_) owner_->abandon(std::move(*this));
}

Result<std::unique_ptr<Renderer>> Renderer::create(Config config) {
    if (config.device == nullptr || config.max_frames == 0 || config.max_textures == 0 ||
        config.sample_count == 0)
        return Error{ErrorCode::invalid_argument,
            "D3D12 renderer requires a device, frames, textures and samples"};
    if (config.max_frames > detail::FrameTracker::kMaxSlots)
        return Error{ErrorCode::invalid_argument, "D3D12 renderer supports at most 64 frame slots"};
    if (config.rtv_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        config.rtv_format != DXGI_FORMAT_B8G8R8A8_UNORM)
        return Error{ErrorCode::unsupported, "D3D12 target must be RGBA8 or BGRA8 UNORM"};
    auto ranges = detail::DescriptorRanges::create(config.max_frames, config.max_textures,
        kShaderVisibleHeapLimit);
    if (auto* error = std::get_if<Error>(&ranges)) return std::move(*error);

    ID3D12Device* device = config.device;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
    levels.NumFeatureLevels = 1;
    levels.pFeatureLevelsRequested = requested;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))) ||
        levels.MaxSupportedFeatureLevel < D3D_FEATURE_LEVEL_11_0)
        return Error{ErrorCode::unsupported, "D3D12 renderer requires feature level 11_0"};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT target_support{config.rtv_format};
    constexpr auto target_needs = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &target_support,
            sizeof(target_support))) ||
        (target_support.Support1 & target_needs) != target_needs)
        return Error{ErrorCode::unsupported, "D3D12 target format is not a blendable render target"};
    if (config.sample_count > 1) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
        quality.Format = config.rtv_format;
        quality.SampleCount = config.sample_count;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &quality, sizeof(quality))) || quality.NumQualityLevels == 0)
            return Error{ErrorCode::unsupported, "D3D12 target sample count is unsupported"};
    }

    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->device = device;
    impl->descriptors = std::get<detail::DescriptorRanges>(std::move(ranges));
    impl->tracker = detail::FrameTracker(config.max_frames);
    impl->slots.resize(config.max_frames);

    auto vs = compile_shader("VsMain", "vs_5_0");
    if (auto* error = std::get_if<Error>(&vs)) return std::move(*error);
    auto ps = compile_shader("PsMain", "ps_5_0");
    if (auto* error = std::get_if<Error>(&ps)) return std::move(*error);
    const auto& vs_blob = std::get<ComPtr<ID3DBlob>>(vs);
    const auto& ps_blob = std::get<ComPtr<ID3DBlob>>(ps);

    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 1;
    texture_range.BaseShaderRegister = 0;
    texture_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[kTextureTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[kTextureTable].DescriptorTable.NumDescriptorRanges = 1;
    parameters[kTextureTable].DescriptorTable.pDescriptorRanges = &texture_range;
    parameters[kTextureTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[kViewportConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kViewportConstants].Constants.ShaderRegister = 0;
    parameters[kViewportConstants].Constants.Num32BitValues = 4;
    parameters[kViewportConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[kModeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kModeConstants].Constants.ShaderRegister = 1;
    parameters[kModeConstants].Constants.Num32BitValues = 4;
    parameters[kModeConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 3;
    root.pParameters = parameters;
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (FAILED(hr)) return hr_error(hr, "D3D12SerializeRootSignature failed");
    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&impl->root));
    if (FAILED(hr)) return hr_error(hr, "CreateRootSignature failed");

    constexpr D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = impl->root.Get();
    pso.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.StencilEnable = FALSE;
    const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.FrontFace = keep;
    pso.DepthStencilState.BackFace = keep;
    pso.InputLayout = {elements, 3};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = config.rtv_format;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = config.sample_count;
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&impl->pipeline));
    if (FAILED(hr)) return hr_error(hr, "CreateGraphicsPipelineState failed");

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = impl->descriptors.total();
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&impl->heap));
    if (FAILED(hr)) return hr_error(hr, "CreateDescriptorHeap failed");
    impl->descriptor_size =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    impl->heap_cpu = impl->heap->GetCPUDescriptorHandleForHeapStart();
    impl->heap_gpu = impl->heap->GetGPUDescriptorHandleForHeapStart();

    auto renderer = std::unique_ptr<Renderer>(new Renderer);
    renderer->impl_ = std::move(impl);
    return renderer;
}

Result<PreparedFrame> Renderer::prepare(FrameSlot slot, ID3D12GraphicsCommandList* list,
    const DrawPacket& packet) {
    if (impl_ && impl_->lost) return lost_error();
    if (!impl_ || impl_->closed)
        return Error{ErrorCode::invalid_frame, "D3D12 renderer is shut down"};
    auto& impl = *impl_;
    if (slot.index >= impl.slots.size() || list == nullptr || slot.fence == nullptr)
        return Error{ErrorCode::invalid_argument, "Invalid D3D12 frame slot or command list"};
    // A completed submission frees its slot without a separate collect().
    if (impl.tracker.state(slot.index) == detail::TicketState::submitted) {
        if (const auto error = impl.poll(slot.index); error && error->code == ErrorCode::device_lost)
            return *error;
    }
    if (impl.tracker.state(slot.index) != detail::TicketState::free)
        return Error{ErrorCode::busy, "D3D12 frame slot is in use"};
    const std::uint64_t completed = impl.completed_value(slot.fence);
    if (completed == std::numeric_limits<std::uint64_t>::max()) {
        impl.lose();
        return lost_error();
    }
    if (slot.completion_value <= completed)
        return Error{ErrorCode::invalid_frame, "D3D12 completion value is already reached"};
    if (!packet) return Error{ErrorCode::invalid_packet, "Null D3D12 draw packet"};
    if (const auto error = validate_packet(*packet)) return *error;
    if (packet->textures.size() > impl.config.max_textures)
        return Error{ErrorCode::capacity, "D3D12 frame has more textures than its SRV range"};

    // Size this frame's uploads before anything is created or recorded.
    impl.expire_dead();
    const auto& textures = packet->textures;
    const std::uint64_t vertex_bytes = packet->vertices.size() * sizeof(Vertex);
    const std::uint64_t index_bytes = packet->indices.size() * sizeof(std::uint32_t);
    std::uint64_t required = *detail::align_up(vertex_bytes, detail::kPlacementAlignment) +
        *detail::align_up(index_bytes, detail::kPlacementAlignment);
    for (const auto& snapshot : textures) {
        const auto& texture = *snapshot;
        const auto footprint = detail::texture_footprint(texture.width, texture.height,
            D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
        if (auto* error = std::get_if<Error>(&footprint)) return *error;
        if (impl.tracker.resident(slot.index,
                texture_key(texture))) continue;
        required += *detail::align_up(std::get<detail::Footprint>(footprint).bytes,
            detail::kPlacementAlignment);
    }

    auto& entry = impl.slots[slot.index];
    // The slot is free, so every earlier upload in its ring has retired and
    // the ring restarts at offset 0. The aligned sum then always fits without
    // wrapping; the old buffer can be replaced because no GPU work uses it.
    if (required > 0 && (!entry.upload || required > entry.ring.capacity())) {
        const std::uint64_t capacity = std::max({kInitialUploadBytes, required,
            entry.ring.capacity() * 2});
        const D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        const auto desc = buffer_desc(capacity);
        ComPtr<ID3D12Resource> buffer;
        HRESULT hr = impl.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) {
            if (removed(hr)) impl.lose();
            return hr_error(hr, "Create D3D12 upload buffer failed");
        }
        void* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        hr = buffer->Map(0, &none, &mapped);
        if (FAILED(hr)) {
            if (removed(hr)) impl.lose();
            return hr_error(hr, "Map D3D12 upload buffer failed");
        }
        entry.upload = std::move(buffer);
        entry.mapped = static_cast<std::byte*>(mapped);
    }
    entry.ring.reset(entry.upload ? entry.upload->GetDesc().Width : 0);

    if (const auto error = impl.tracker.prepare(slot.index, slot.completion_value)) return *error;
    (void)impl.descriptors.reset(slot.index);
    const D3D12_GPU_VIRTUAL_ADDRESS base = entry.upload ? entry.upload->GetGPUVirtualAddress() : 0;
    impl.copies.clear();
    auto fail = [&](Error error) -> Result<PreparedFrame> {
        // Nothing was recorded yet: drop the slot and every texture it created.
        (void)impl.tracker.abandon(slot.index);
        impl.retire_textures();
        if (error.code == ErrorCode::device_lost) impl.lose();
        return error;
    };
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto& texture = *textures[i];
        auto used = impl.tracker.use(slot.index,
            texture_key(texture));
        if (auto* error = std::get_if<Error>(&used)) return fail(std::move(*error));
        const auto use = std::get<detail::FrameTracker::Use>(used);
        if (impl.natives.size() < impl.tracker.entry_count())
            impl.natives.resize(impl.tracker.entry_count());
        auto& native = impl.natives[use.entry];
        native.source = textures[i];
        if (use.created) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = texture.width;
            desc.Height = texture.height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
            const HRESULT hr = impl.device->CreateCommittedResource(&default_heap,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&native.texture));
            if (FAILED(hr)) return fail(hr_error(hr, "Create D3D12 texture failed"));
            const auto footprint = std::get<detail::Footprint>(detail::texture_footprint(
                texture.width, texture.height, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));
            auto offset = entry.ring.allocate(footprint.bytes, detail::kPlacementAlignment);
            if (auto* error = std::get_if<Error>(&offset)) return fail(std::move(*error));
            const auto at = std::get<std::uint64_t>(offset);
            const std::size_t row = static_cast<std::size_t>(texture.width) * 4;
            for (std::uint32_t y = 0; y < texture.height; ++y)
                std::memcpy(entry.mapped + at + y * footprint.row_pitch,
                    texture.rgba.data() + y * row, row);
            impl.copies.push_back({use.entry, at, footprint, texture.width, texture.height});
        }
        const auto descriptor = impl.descriptors.allocate(slot.index);
        if (auto* error = std::get_if<Error>(&descriptor)) return fail(*error);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = impl.heap_cpu;
        handle.ptr += static_cast<SIZE_T>(std::get<std::uint32_t>(descriptor)) * impl.descriptor_size;
        impl.device->CreateShaderResourceView(native.texture.Get(), &view, handle);
    }

    entry.vertices = {};
    entry.indices = {};
    if (vertex_bytes) {
        auto offset = entry.ring.allocate(vertex_bytes, detail::kPlacementAlignment);
        if (auto* error = std::get_if<Error>(&offset)) return fail(std::move(*error));
        const auto at = std::get<std::uint64_t>(offset);
        std::memcpy(entry.mapped + at, packet->vertices.data(), static_cast<std::size_t>(vertex_bytes));
        entry.vertices = {base + at, static_cast<UINT>(vertex_bytes), sizeof(Vertex)};
    }
    if (index_bytes) {
        auto offset = entry.ring.allocate(index_bytes, detail::kPlacementAlignment);
        if (auto* error = std::get_if<Error>(&offset)) return fail(std::move(*error));
        const auto at = std::get<std::uint64_t>(offset);
        std::memcpy(entry.mapped + at, packet->indices.data(), static_cast<std::size_t>(index_bytes));
        entry.indices = {base + at, static_cast<UINT>(index_bytes), DXGI_FORMAT_R32_UINT};
    }

    // Every allocation succeeded: record the copies and their transitions.
    impl.barriers.clear();
    for (const auto& copy : impl.copies) {
        auto* texture = impl.natives[copy.entry].texture.Get();
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = entry.upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = copy.offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, copy.width, copy.height, 1,
            static_cast<UINT>(copy.footprint.row_pitch)};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        impl.barriers.push_back(barrier);
    }
    if (!impl.barriers.empty())
        list->ResourceBarrier(static_cast<UINT>(impl.barriers.size()), impl.barriers.data());

    entry.packet = packet;
    entry.fence = slot.fence;
    return PreparedFrame(this, slot.index);
}

Status Renderer::record(const PreparedFrame& prepared, ID3D12GraphicsCommandList* list,
    Target target) {
    if (impl_ && impl_->lost) return lost_error();
    if (!impl_ || impl_->closed || prepared.owner_ != this ||
        prepared.index_ >= impl_->slots.size())
        return Error{ErrorCode::invalid_frame, "D3D12 frame ticket does not belong to renderer"};
    auto& impl = *impl_;
    auto& slot = impl.slots[prepared.index_];
    if (impl.tracker.state(prepared.index_) != detail::TicketState::prepared || !slot.packet)
        return Error{ErrorCode::invalid_frame, "D3D12 frame is not prepared for recording"};
    const auto& packet = *slot.packet;
    const double width = static_cast<double>(packet.viewport.w) * packet.scale;
    const double height = static_cast<double>(packet.viewport.h) * packet.scale;
    if (list == nullptr || target.rtv.ptr == 0 || target.width == 0 || target.height == 0 ||
        target.width > D3D12_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH ||
        target.height > D3D12_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH ||
        !(width > 0) || !(height > 0) ||
        std::round(width) != target.width || std::round(height) != target.height)
        return Error{ErrorCode::invalid_argument, "Incompatible D3D12 command list or render target"};
    if (const auto error = impl.tracker.record(prepared.index_)) return error;
    if (packet.batches.empty() || packet.indices.empty()) return std::nullopt;

    list->SetGraphicsRootSignature(impl.root.Get());
    list->SetPipelineState(impl.pipeline.Get());
    ID3D12DescriptorHeap* heaps[] = {impl.heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &slot.vertices);
    list->IASetIndexBuffer(&slot.indices);
    list->OMSetRenderTargets(1, &target.rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(target.width),
        static_cast<float>(target.height), 0, 1};
    list->RSSetViewports(1, &viewport);
    const ViewportConstants constants{packet.viewport.x, packet.viewport.y,
        packet.viewport.w, packet.viewport.h};
    list->SetGraphicsRoot32BitConstants(kViewportConstants, 4, &constants, 0);
    const std::uint32_t first = impl.descriptors.first(prepared.index_);
    for (const auto& batch : packet.batches) {
        if (!batch.index_count || batch.clip.w <= 0 || batch.clip.h <= 0) continue;
        const auto rect = ::wui::detail::physical_scissor(batch.clip, packet.viewport, packet.scale,
            target.width, target.height);
        if (rect.empty()) continue;
        const D3D12_RECT scissor{rect.left, rect.top, rect.right, rect.bottom};
        list->RSSetScissorRects(1, &scissor);
        D3D12_GPU_DESCRIPTOR_HANDLE texture = impl.heap_gpu;
        texture.ptr += static_cast<UINT64>(first + batch.texture_index) * impl.descriptor_size;
        list->SetGraphicsRootDescriptorTable(kTextureTable, texture);
        const ModeConstants mode{
            packet.textures[batch.texture_index]->alpha == AlphaMode::straight ? 1u : 0u};
        list->SetGraphicsRoot32BitConstants(kModeConstants, 4, &mode, 0);
        list->DrawIndexedInstanced(batch.index_count, 1, batch.first_index, batch.base_vertex, 0);
    }
    return std::nullopt;
}

void Renderer::submitted(PreparedFrame&& prepared) noexcept {
    if (!impl_ || impl_->closed || prepared.owner_ != this ||
        prepared.index_ >= impl_->slots.size()) return;
    if (impl_->tracker.state(prepared.index_) != detail::TicketState::recorded) return;
    (void)impl_->tracker.submit(prepared.index_); // Guarded valid transition.
    prepared.owner_ = nullptr;
}

void Renderer::abandon(PreparedFrame&& prepared) noexcept {
    if (!impl_ || prepared.owner_ != this || prepared.index_ >= impl_->slots.size()) return;
    const auto state = impl_->tracker.state(prepared.index_);
    if (state == detail::TicketState::prepared || state == detail::TicketState::recorded) {
        (void)impl_->tracker.abandon(prepared.index_);
        impl_->release_slot(prepared.index_);
        // The lists never executed, so uploads created for them free at once.
        try {
            impl_->retire_textures();
        } catch (...) {
            // retire_textures only grows a reserved free list; retried by collect().
        }
    }
    prepared.owner_ = nullptr;
}

Status Renderer::collect() {
    if (impl_ && impl_->lost) return lost_error();
    if (!impl_ || impl_->closed) return std::nullopt;
    auto& impl = *impl_;
    for (std::uint32_t i = 0; i < impl.slots.size(); ++i) {
        if (impl.tracker.state(i) != detail::TicketState::submitted) continue;
        if (const auto error = impl.poll(i); error && error->code != ErrorCode::busy) return error;
    }
    impl.retire_textures();
    return std::nullopt;
}

Status Renderer::shutdown() {
    if (impl_ && impl_->lost) {
        impl_->closed = true; // Terminal device removal: never poll fences again.
        return lost_error();
    }
    if (!impl_ || impl_->closed) return std::nullopt;
    if (const auto error = collect()) {
        if (error->code == ErrorCode::device_lost) impl_->closed = true;
        return error;
    }
    for (std::uint32_t i = 0; i < impl_->slots.size(); ++i)
        if (impl_->tracker.state(i) != detail::TicketState::free)
            return Error{ErrorCode::busy, "D3D12 frame slots still in use"};
    impl_->closed = true;
    return std::nullopt;
}

Renderer::~Renderer() {
    if (impl_ && !impl_->closed) {
        const auto status = shutdown();
        assert((!status || status->code == ErrorCode::device_lost) &&
            "Host must complete or abandon every D3D12 frame before destruction");
        (void)status;
    }
}

std::vector<detail::ResidentTexture> detail::Inspection::resident(const Renderer& renderer) {
    std::vector<ResidentTexture> out;
    if (!renderer.impl_) return out;
    const auto& impl = *renderer.impl_;
    for (std::uint32_t i = 0; i < impl.natives.size(); ++i) {
        if (!impl.tracker.live(i)) continue;
        out.push_back({impl.tracker.key(i), impl.natives[i].texture.Get(),
            impl.tracker.references(i)});
    }
    return out;
}

detail::CompletedValueFn detail::Inspection::replace_completed_value(Renderer& renderer,
    CompletedValueFn replacement) noexcept {
    return std::exchange(renderer.impl_->completed_value, replacement);
}

} // namespace wui::d3d12
