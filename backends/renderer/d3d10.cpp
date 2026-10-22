// Isolated D3D10 renderer adapted from the D3D11 reference: consumes
// immutable DrawPackets on the caller's thread. No windows, swap chains or
// worker threads. Encoded-SDR baseline, premultiplied ONE/INV_SRC_ALPHA
// blending, no sRGB conversion. ID3D10Device is also the immediate context.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// d3d10_1.h must precede d3d10.h (pulled in by the public header).
#include <d3d10_1.h>
#include <wui/backends/d3d10.hpp>

#include "d3d10_state.hpp"
#include "scissor.hpp"
#include "wui_d3d10_shader_source.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

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

namespace wui::d3d10 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxInFlight = 3;

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

Error hr_error(HRESULT hr, const char* what) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), " (hr=0x%08lX)",
        static_cast<unsigned long>(hr));
    return make_error(ErrorCode::backend_failure,
        std::string(what) + buffer);
}

// A plain ID3D10Device is a 10_0 device by definition. A 10.1 runtime
// device can also be created at 9_x levels; those are rejected.
Status check_feature_level(ID3D10Device* device) {
    ComPtr<ID3D10Device1> device1;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1))) &&
        device1->GetFeatureLevel() < D3D10_FEATURE_LEVEL_10_0) {
        return make_error(ErrorCode::unsupported,
            "D3D10 renderer requires feature level 10_0");
    }
    return std::nullopt;
}

struct ViewportConstants {
    float origin_x = 0, origin_y = 0, size_w = 1, size_h = 1;
};

struct ModeConstants {
    std::int32_t straight = 1;
    float pad[3] = {};
};

static_assert(sizeof(ViewportConstants) == 16);
static_assert(sizeof(ModeConstants) == 16);

struct TextureEntry {
    std::uint32_t slot = 0, generation = 0;
    std::uint64_t revision = 0;
    std::uint32_t width = 0, height = 0;
    AlphaMode alpha = AlphaMode::straight;
    std::weak_ptr<const TextureSnapshot> source;
    ComPtr<ID3D10Texture2D> texture;
    ComPtr<ID3D10ShaderResourceView> srv;
};

struct Submission {
    DrawPacket packet;
    ComPtr<ID3D10Query> query;
};

struct RestoreGuard {
    ID3D10Device* device = nullptr;
    const detail::Saved* saved = nullptr;
    bool active = false;
    ~RestoreGuard() {
        if (active) detail::restore_state(device, *saved);
    }
};

Result<ComPtr<ID3DBlob>> compile_shader(
    const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(detail::kShaderSource,
        sizeof(detail::kShaderSource) - 1, "wui_d3d10", nullptr, nullptr,
        entry, target, flags, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        std::string message("D3D10 shader compilation failed for ");
        message += entry;
        if (errors && errors->GetBufferPointer() != nullptr) {
            message += ": ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        return make_error(ErrorCode::backend_failure, std::move(message));
    }
    return code;
}

template <class T>
bool write_dynamic(ID3D10Buffer* buffer, const T& value) {
    void* mapped = nullptr;
    if (FAILED(buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    *static_cast<T*>(mapped) = value;
    buffer->Unmap();
    return true;
}

} // namespace

struct Renderer::Impl {
    ComPtr<ID3D10Device> device;
    ComPtr<ID3D10VertexShader> vertex_shader;
    ComPtr<ID3D10PixelShader> pixel_shader;
    ComPtr<ID3D10InputLayout> layout;
    ComPtr<ID3D10Buffer> vertex_buffer;
    std::size_t vertex_capacity = 0;
    ComPtr<ID3D10Buffer> index_buffer;
    std::size_t index_capacity = 0;
    ComPtr<ID3D10Buffer> viewport_cbuf;
    ComPtr<ID3D10Buffer> mode_cbuf;
    ComPtr<ID3D10SamplerState> sampler;
    ComPtr<ID3D10BlendState> blend;
    ComPtr<ID3D10RasterizerState> rasterizer;
    ComPtr<ID3D10DepthStencilState> depth_stencil;
    std::vector<TextureEntry> textures;
    std::vector<Submission> in_flight;
    // Retired event queries reused by later submissions.
    std::vector<ComPtr<ID3D10Query>> spare_queries;
};

Status Renderer::init_device_objects(ID3D10Device* device, Impl& impl) {
    auto compiled_vs = compile_shader("VsMain", "vs_4_0");
    if (std::holds_alternative<Error>(compiled_vs)) {
        return std::get<Error>(std::move(compiled_vs));
    }
    auto compiled_ps = compile_shader("PsMain", "ps_4_0");
    if (std::holds_alternative<Error>(compiled_ps)) {
        return std::get<Error>(std::move(compiled_ps));
    }
    const auto& vs_blob = std::get<ComPtr<ID3DBlob>>(compiled_vs);
    const auto& ps_blob = std::get<ComPtr<ID3DBlob>>(compiled_ps);
    HRESULT hr = device->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        impl.vertex_shader.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateVertexShader failed");
    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(),
        ps_blob->GetBufferSize(), impl.pixel_shader.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreatePixelShader failed");

    constexpr D3D10_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
            D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(elements, 3, vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(), impl.layout.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateInputLayout failed");

    auto make_cbuf = [&](ComPtr<ID3D10Buffer>& out) -> Status {
        D3D10_BUFFER_DESC desc{};
        desc.ByteWidth = 16;
        desc.Usage = D3D10_USAGE_DYNAMIC;
        desc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
        const HRESULT created = device->CreateBuffer(
            &desc, nullptr, out.ReleaseAndGetAddressOf());
        if (FAILED(created)) return hr_error(created, "CreateBuffer failed");
        return std::nullopt;
    };
    if (const auto error = make_cbuf(impl.viewport_cbuf)) return error;
    if (const auto error = make_cbuf(impl.mode_cbuf)) return error;

    D3D10_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D10_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D10_FLOAT32_MAX;
    hr = device->CreateSamplerState(
        &sampler_desc, impl.sampler.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateSamplerState failed");

    D3D10_BLEND_DESC blend_desc{};
    blend_desc.BlendEnable[0] = TRUE;
    blend_desc.SrcBlend = D3D10_BLEND_ONE;
    blend_desc.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
    blend_desc.BlendOp = D3D10_BLEND_OP_ADD;
    blend_desc.SrcBlendAlpha = D3D10_BLEND_ONE;
    blend_desc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
    blend_desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    blend_desc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(
        &blend_desc, impl.blend.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateBlendState failed");

    D3D10_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D10_FILL_SOLID;
    raster_desc.CullMode = D3D10_CULL_NONE;
    raster_desc.ScissorEnable = TRUE;
    raster_desc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(
        &raster_desc, impl.rasterizer.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateRasterizerState failed");

    D3D10_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(
        &depth_desc, impl.depth_stencil.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateDepthStencilState failed");
    return std::nullopt;
}

namespace {

Status ensure_buffer(ID3D10Device* device, ComPtr<ID3D10Buffer>& buffer,
    std::size_t& capacity, std::size_t required, D3D10_BIND_FLAG bind) {
    if (required <= capacity && buffer) return std::nullopt;
    std::size_t grown = capacity != 0 ? capacity * 2 : 4096;
    if (grown < required) grown = required;
    D3D10_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(grown);
    desc.Usage = D3D10_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    ComPtr<ID3D10Buffer> created;
    const HRESULT hr = device->CreateBuffer(&desc, nullptr, &created);
    if (FAILED(hr)) return hr_error(hr, "CreateBuffer failed");
    buffer = std::move(created);
    capacity = grown;
    return std::nullopt;
}

// Native textures are keyed by snapshot identity, not just by handle and
// revision: snapshots from different TextureStores can share handle values,
// and reusing a texture across them would sample the wrong pixels.
bool same_texture(const TextureEntry& entry, const TextureSnapshot& snapshot) {
    return entry.slot == snapshot.handle.slot &&
        entry.generation == snapshot.handle.generation &&
        entry.revision == snapshot.revision &&
        entry.width == snapshot.width &&
        entry.height == snapshot.height &&
        entry.alpha == snapshot.alpha &&
        entry.source.lock().get() == &snapshot;
}

TextureEntry* find_texture(
    std::vector<TextureEntry>& entries, const TextureSnapshot& snapshot) {
    for (auto& entry : entries) {
        if (same_texture(entry, snapshot)) return &entry;
    }
    return nullptr;
}

Status upload_texture(ID3D10Device* device,
    std::vector<TextureEntry>& entries,
    const std::shared_ptr<const TextureSnapshot>& snapshot) {
    if (find_texture(entries, *snapshot) != nullptr) return std::nullopt;
    // Each snapshot gets its own immutable texture, so a newer
    // revision never changes what an older retained packet samples.
    D3D10_TEXTURE2D_DESC desc{};
    desc.Width = snapshot->width;
    desc.Height = snapshot->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D10_USAGE_IMMUTABLE;
    desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    D3D10_SUBRESOURCE_DATA initial{};
    initial.pSysMem = snapshot->rgba.data();
    initial.SysMemPitch = snapshot->width * 4;
    TextureEntry entry;
    entry.slot = snapshot->handle.slot;
    entry.generation = snapshot->handle.generation;
    entry.revision = snapshot->revision;
    entry.width = snapshot->width;
    entry.height = snapshot->height;
    entry.alpha = snapshot->alpha;
    entry.source = snapshot;
    HRESULT hr = device->CreateTexture2D(
        &desc, &initial, entry.texture.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateTexture2D failed");
    D3D10_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(
        entry.texture.Get(), &srv_desc, entry.srv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateShaderResourceView failed");
    entries.push_back(std::move(entry));
    return std::nullopt;
}

} // namespace

Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
Renderer::~Renderer() = default;

Result<std::unique_ptr<Renderer>> Renderer::create(ID3D10Device* device) {
    if (device == nullptr) {
        return make_error(ErrorCode::invalid_argument,
            "D3D10 renderer requires a device");
    }
    if (auto error = check_feature_level(device)) return std::move(*error);
    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->device = device;
    if (auto error = init_device_objects(device, *impl)) {
        return std::move(*error);
    }
    auto renderer = std::unique_ptr<Renderer>(new Renderer());
    renderer->impl_ = std::move(impl);
    return renderer;
}

Status Renderer::render(const DrawPacket& packet, Target target,
    StatePolicy policy) {
    if (!impl_) {
        return make_error(ErrorCode::invalid_frame,
            "D3D10 renderer is moved or uninitialized");
    }
    if (!packet) {
        return make_error(ErrorCode::invalid_argument,
            "D3D10 render requires a packet");
    }
    if (target.view == nullptr || target.width == 0 || target.height == 0) {
        return make_error(ErrorCode::invalid_argument,
            "D3D10 render requires a valid target");
    }
    if (const auto error = validate_packet(*packet)) {
        return make_error(ErrorCode::invalid_packet,
            std::string("Invalid draw packet: ") + error->message);
    }
    const auto expected_w =
        static_cast<std::int64_t>(std::llround(packet->viewport.w * packet->scale));
    const auto expected_h =
        static_cast<std::int64_t>(std::llround(packet->viewport.h * packet->scale));
    if (expected_w != target.width || expected_h != target.height) {
        return make_error(ErrorCode::invalid_argument,
            "Packet viewport and scale do not match target dimensions");
    }
    ID3D10Device* device = impl_->device.Get();
    if (FAILED(device->GetDeviceRemovedReason())) {
        return make_error(ErrorCode::device_lost, "D3D10 device removed");
    }
    if (impl_->in_flight.size() >= kMaxInFlight) {
        return make_error(ErrorCode::busy,
            "D3D10 renderer has 3 frames in flight");
    }
    {
        ComPtr<ID3D10Resource> resource;
        target.view->GetResource(resource.GetAddressOf());
        ComPtr<ID3D10Texture2D> color;
        if (!resource || FAILED(resource.As(&color)) || !color) {
            return make_error(ErrorCode::invalid_argument,
                "D3D10 target view is not a 2D texture");
        }
        D3D10_TEXTURE2D_DESC desc{};
        color->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            return make_error(ErrorCode::unsupported,
                "D3D10 target must be single-sample RGBA8 or BGRA8 UNORM");
        }
        if (desc.SampleDesc.Count != 1) {
            return make_error(ErrorCode::unsupported,
                "D3D10 renderer accepts single-sample targets only");
        }
        if (desc.Width != target.width || desc.Height != target.height) {
            return make_error(ErrorCode::invalid_argument,
                "D3D10 target dimensions do not match its texture");
        }
    }
    const std::uint64_t vertex_bytes =
        static_cast<std::uint64_t>(packet->vertices.size()) * sizeof(Vertex);
    const std::uint64_t index_bytes =
        static_cast<std::uint64_t>(packet->indices.size()) * sizeof(std::uint32_t);
    if (vertex_bytes > std::numeric_limits<UINT>::max() ||
        index_bytes > std::numeric_limits<UINT>::max()) {
        return make_error(ErrorCode::invalid_packet,
            "Packet buffer byte size exceeds API range");
    }

    detail::Saved saved;
    RestoreGuard guard;
    if (policy == StatePolicy::restore) {
        detail::save_state(device, saved);
        guard.device = device;
        guard.saved = &saved;
        guard.active = true;
    }

    // Unbind the host's sampled texture first so the UI target is never
    // simultaneously bound for read and write.
    ID3D10ShaderResourceView* null_srv = nullptr;
    device->PSSetShaderResources(0, 1, &null_srv);
    ID3D10RenderTargetView* rtv = target.view;
    device->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D10_VIEWPORT viewport{0, 0, target.width, target.height, 0, 1};
    device->RSSetViewports(1, &viewport);
    device->IASetInputLayout(impl_->layout.Get());
    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device->VSSetShader(impl_->vertex_shader.Get());
    device->GSSetShader(nullptr);
    device->PSSetShader(impl_->pixel_shader.Get());
    device->PSSetSamplers(0, 1, impl_->sampler.GetAddressOf());
    constexpr FLOAT blend_factor[4] = {0, 0, 0, 0};
    device->OMSetBlendState(impl_->blend.Get(), blend_factor, 0xffffffff);
    device->RSSetState(impl_->rasterizer.Get());
    device->OMSetDepthStencilState(impl_->depth_stencil.Get(), 0);

    // From here every early return restores host state through the guard.
    const ViewportConstants viewport_value{
        packet->viewport.x, packet->viewport.y,
        packet->viewport.w, packet->viewport.h};
    if (!write_dynamic(impl_->viewport_cbuf.Get(), viewport_value)) {
        return hr_error(E_FAIL, "Map viewport constants failed");
    }
    device->VSSetConstantBuffers(0, 1, impl_->viewport_cbuf.GetAddressOf());

    if (const auto error = ensure_buffer(device, impl_->vertex_buffer,
            impl_->vertex_capacity, static_cast<std::size_t>(vertex_bytes),
            D3D10_BIND_VERTEX_BUFFER)) {
        return std::move(*error);
    }
    if (const auto error = ensure_buffer(device, impl_->index_buffer,
            impl_->index_capacity, static_cast<std::size_t>(index_bytes),
            D3D10_BIND_INDEX_BUFFER)) {
        return std::move(*error);
    }
    if (!packet->vertices.empty()) {
        void* mapped = nullptr;
        if (FAILED(impl_->vertex_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped))) {
            return hr_error(E_FAIL, "Map vertex buffer failed");
        }
        std::memcpy(mapped, packet->vertices.data(),
            static_cast<std::size_t>(vertex_bytes));
        impl_->vertex_buffer->Unmap();
    }
    if (!packet->indices.empty()) {
        void* mapped = nullptr;
        if (FAILED(impl_->index_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped))) {
            return hr_error(E_FAIL, "Map index buffer failed");
        }
        std::memcpy(mapped, packet->indices.data(),
            static_cast<std::size_t>(index_bytes));
        impl_->index_buffer->Unmap();
    }
    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    device->IASetVertexBuffers(
        0, 1, impl_->vertex_buffer.GetAddressOf(), &stride, &offset);
    device->IASetIndexBuffer(
        impl_->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    // Atlas uploads precede every draw that refers to them.
    for (const auto& snapshot : packet->textures) {
        if (const auto error = upload_texture(device, impl_->textures, snapshot)) {
            return std::move(*error);
        }
    }

    for (const auto& batch : packet->batches) {
        const auto& snapshot = packet->textures[batch.texture_index];
        const TextureEntry* bound = find_texture(impl_->textures, *snapshot);
        if (bound == nullptr) {
            return make_error(ErrorCode::backend_failure,
                "Missing uploaded texture revision");
        }
        const auto pixels = ::wui::detail::physical_scissor(batch.clip, packet->viewport,
            packet->scale, target.width, target.height);
        if (pixels.empty() || batch.index_count == 0) continue;
        const D3D10_RECT scissor{pixels.left, pixels.top, pixels.right, pixels.bottom};
        device->RSSetScissorRects(1, &scissor);
        ID3D10ShaderResourceView* srv = bound->srv.Get();
        device->PSSetShaderResources(0, 1, &srv);
        const ModeConstants mode{bound->alpha == AlphaMode::straight ? 1 : 0};
        if (!write_dynamic(impl_->mode_cbuf.Get(), mode)) {
            return hr_error(E_FAIL, "Map texture-mode constants failed");
        }
        device->PSSetConstantBuffers(1, 1, impl_->mode_cbuf.GetAddressOf());
        device->DrawIndexed(batch.index_count, batch.first_index,
            batch.base_vertex);
    }
    device->PSSetShaderResources(0, 1, &null_srv);

    ComPtr<ID3D10Query> query;
    if (!impl_->spare_queries.empty()) {
        query = std::move(impl_->spare_queries.back());
        impl_->spare_queries.pop_back();
    } else {
        D3D10_QUERY_DESC query_desc{};
        query_desc.Query = D3D10_QUERY_EVENT;
        const HRESULT hr = device->CreateQuery(&query_desc, query.GetAddressOf());
        if (FAILED(hr)) return hr_error(hr, "CreateQuery failed");
    }
    query->End();
    impl_->in_flight.push_back(Submission{packet, std::move(query)});
    return std::nullopt;
}

Status Renderer::collect() {
    if (!impl_) {
        return make_error(ErrorCode::invalid_frame,
            "D3D10 renderer is moved or uninitialized");
    }
    if (FAILED(impl_->device->GetDeviceRemovedReason())) {
        return make_error(ErrorCode::device_lost, "D3D10 device removed");
    }
    // Nonblocking, nonflushing poll in submission order.
    while (!impl_->in_flight.empty()) {
        auto& front = impl_->in_flight.front();
        const HRESULT hr = front.query->GetData(
            nullptr, 0, D3D10_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            impl_->spare_queries.push_back(std::move(front.query));
            impl_->in_flight.erase(impl_->in_flight.begin());
        } else if (hr == S_FALSE) {
            break;
        } else {
            return hr_error(hr, "D3D10 query poll failed");
        }
    }
    auto referenced_by_in_flight = [&](const TextureEntry& entry) {
        for (const auto& submission : impl_->in_flight) {
            for (const auto& snapshot : submission.packet->textures) {
                if (same_texture(entry, *snapshot)) return true;
            }
        }
        return false;
    };
    // A revision is released once no live snapshot and no unretired
    // submission refers to it.
    for (auto it = impl_->textures.begin(); it != impl_->textures.end();) {
        if (it->source.expired() && !referenced_by_in_flight(*it)) {
            it = impl_->textures.erase(it);
        } else {
            ++it;
        }
    }
    return std::nullopt;
}

Status Renderer::recreate(ID3D10Device* replacement_device) {
    if (!impl_) {
        return make_error(ErrorCode::invalid_frame,
            "D3D10 renderer is moved or uninitialized");
    }
    if (replacement_device == nullptr) {
        return make_error(ErrorCode::invalid_argument,
            "D3D10 recreate requires a replacement device");
    }
    // An unsupported replacement is rejected before anything is released,
    // so the renderer stays usable on its current device.
    if (auto error = check_feature_level(replacement_device)) return error;
    // The caller stopped using the old device: drop every UI-owned resource
    // tied to it, including pending submissions, then rebuild textures
    // lazily from retained CPU snapshots on the next render.
    impl_->vertex_shader.Reset();
    impl_->pixel_shader.Reset();
    impl_->layout.Reset();
    impl_->vertex_buffer.Reset();
    impl_->vertex_capacity = 0;
    impl_->index_buffer.Reset();
    impl_->index_capacity = 0;
    impl_->viewport_cbuf.Reset();
    impl_->mode_cbuf.Reset();
    impl_->sampler.Reset();
    impl_->blend.Reset();
    impl_->rasterizer.Reset();
    impl_->depth_stencil.Reset();
    impl_->textures.clear();
    impl_->in_flight.clear();
    impl_->spare_queries.clear();
    impl_->device = replacement_device;
    return init_device_objects(replacement_device, *impl_);
}

} // namespace wui::d3d10
