// A9 isolated D3D11 renderer: consumes immutable DrawPackets on the caller's
// thread. No windows, swap chains or worker threads. Encoded-SDR baseline,
// premultiplied ONE/INV_SRC_ALPHA blending. UNORM views store the encoded
// values; _SRGB views get a pixel shader that decodes to linear first so the
// hardware re-encode stores the same values for opaque pixels.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wui/backends/d3d11.hpp>

#include "d3d11_state.hpp"
#include "scissor.hpp"
#include "wui_d3d11_shader_source.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace wui::d3d11 {
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
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
};

struct Submission {
    DrawPacket packet;
    ComPtr<ID3D11Query> query;
};

struct RestoreGuard {
    ID3D11DeviceContext* context = nullptr;
    const detail::Saved* saved = nullptr;
    bool active = false;
    ~RestoreGuard() {
        if (active) detail::restore_state(context, *saved);
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
        sizeof(detail::kShaderSource) - 1, "wui_d3d11", nullptr, nullptr,
        entry, target, flags, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        std::string message("D3D11 shader compilation failed for ");
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
bool write_dynamic(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
    const T& value) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    *static_cast<T*>(mapped.pData) = value;
    context->Unmap(buffer, 0);
    return true;
}

// Render-target view formats the encoded-SDR contract can write. The view
// format decides; the texture may be typeless.
enum class TargetEncoding { unsupported, unorm, srgb };

TargetEncoding target_encoding(DXGI_FORMAT view_format) {
    switch (view_format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return TargetEncoding::unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return TargetEncoding::srgb;
    default:
        return TargetEncoding::unsupported;
    }
}

} // namespace

struct Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11PixelShader> pixel_shader_srgb;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertex_buffer;
    std::size_t vertex_capacity = 0;
    ComPtr<ID3D11Buffer> index_buffer;
    std::size_t index_capacity = 0;
    ComPtr<ID3D11Buffer> viewport_cbuf;
    ComPtr<ID3D11Buffer> mode_cbuf;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    std::vector<TextureEntry> textures;
    std::vector<Submission> in_flight;
};

Status Renderer::init_device_objects(ID3D11Device* device, Impl& impl) {
    auto compiled_vs = compile_shader("VsMain", "vs_5_0");
    if (std::holds_alternative<Error>(compiled_vs)) {
        return std::get<Error>(std::move(compiled_vs));
    }
    auto compiled_ps = compile_shader("PsMain", "ps_5_0");
    if (std::holds_alternative<Error>(compiled_ps)) {
        return std::get<Error>(std::move(compiled_ps));
    }
    auto compiled_ps_srgb = compile_shader("PsMainSrgb", "ps_5_0");
    if (std::holds_alternative<Error>(compiled_ps_srgb)) {
        return std::get<Error>(std::move(compiled_ps_srgb));
    }
    const auto& vs_blob = std::get<ComPtr<ID3DBlob>>(compiled_vs);
    const auto& ps_blob = std::get<ComPtr<ID3DBlob>>(compiled_ps);
    const auto& ps_srgb_blob = std::get<ComPtr<ID3DBlob>>(compiled_ps_srgb);
    HRESULT hr = device->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr,
        impl.vertex_shader.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateVertexShader failed");
    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(),
        ps_blob->GetBufferSize(), nullptr,
        impl.pixel_shader.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreatePixelShader failed");
    hr = device->CreatePixelShader(ps_srgb_blob->GetBufferPointer(),
        ps_srgb_blob->GetBufferSize(), nullptr,
        impl.pixel_shader_srgb.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreatePixelShader (sRGB) failed");

    constexpr D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device->CreateInputLayout(elements, 3, vs_blob->GetBufferPointer(),
        vs_blob->GetBufferSize(), impl.layout.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateInputLayout failed");

    auto make_cbuf = [&](ComPtr<ID3D11Buffer>& out) -> Status {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT created = device->CreateBuffer(
            &desc, nullptr, out.ReleaseAndGetAddressOf());
        if (FAILED(created)) return hr_error(created, "CreateBuffer failed");
        return std::nullopt;
    };
    if (const auto error = make_cbuf(impl.viewport_cbuf)) return error;
    if (const auto error = make_cbuf(impl.mode_cbuf)) return error;

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(
        &sampler_desc, impl.sampler.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateSamplerState failed");

    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(
        &blend_desc, impl.blend.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateBlendState failed");

    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.ScissorEnable = TRUE;
    raster_desc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(
        &raster_desc, impl.rasterizer.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateRasterizerState failed");

    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(
        &depth_desc, impl.depth_stencil.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr_error(hr, "CreateDepthStencilState failed");
    return std::nullopt;
}

namespace {

Status ensure_buffer(ID3D11Device* device, ComPtr<ID3D11Buffer>& buffer,
    std::size_t& capacity, std::size_t required, D3D11_BIND_FLAG bind) {
    if (required <= capacity && buffer) return std::nullopt;
    std::size_t grown = capacity != 0 ? capacity * 2 : 4096;
    if (grown < required) grown = required;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(grown);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> created;
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

const TextureEntry* find_texture(
    const std::vector<TextureEntry>& entries,
    const TextureSnapshot& snapshot) {
    for (const auto& entry : entries) {
        if (same_texture(entry, snapshot)) return &entry;
    }
    return nullptr;
}

TextureEntry* find_texture_mutable(
    std::vector<TextureEntry>& entries,
    const TextureSnapshot& snapshot) {
    for (auto& entry : entries) {
        if (same_texture(entry, snapshot)) return &entry;
    }
    return nullptr;
}

Status upload_texture(ID3D11Device* device,
    std::vector<TextureEntry>& entries,
    const std::shared_ptr<const TextureSnapshot>& snapshot) {
    if (find_texture_mutable(entries, *snapshot) != nullptr) {
        return std::nullopt;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = snapshot->width;
    desc.Height = snapshot->height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
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
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
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

Result<std::unique_ptr<Renderer>> Renderer::create(ID3D11Device* device) {
    if (device == nullptr) {
        return make_error(ErrorCode::invalid_argument,
            "D3D11 renderer requires a device");
    }
    auto impl = std::unique_ptr<Impl>(new Impl());
    impl->device = device;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        return make_error(ErrorCode::unsupported,
            "D3D11 renderer requires feature level 11_0");
    }
    if (const auto error = init_device_objects(device, *impl)) {
        return std::move(*error);
    }
    auto renderer = std::unique_ptr<Renderer>(new Renderer());
    renderer->impl_ = std::move(impl);
    return renderer;
}

Status Renderer::render(ID3D11DeviceContext* context, const DrawPacket& packet,
    Target target, StatePolicy policy) {
    if (!impl_) {
        return make_error(ErrorCode::invalid_frame,
            "D3D11 renderer is moved or uninitialized");
    }
    if (context == nullptr) {
        return make_error(ErrorCode::invalid_argument,
            "D3D11 render requires a device context");
    }
    if (!packet) {
        return make_error(ErrorCode::invalid_argument,
            "D3D11 render requires a packet");
    }
    if (target.view == nullptr || target.width == 0 || target.height == 0) {
        return make_error(ErrorCode::invalid_argument,
            "D3D11 render requires a valid target");
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
    ID3D11Device* device = impl_->device.Get();
    if (FAILED(device->GetDeviceRemovedReason())) {
        return make_error(ErrorCode::device_lost, "D3D11 device removed");
    }
    if (impl_->in_flight.size() >= kMaxInFlight) {
        return make_error(ErrorCode::busy,
            "D3D11 renderer has 3 frames in flight");
    }
    TargetEncoding encoding = TargetEncoding::unsupported;
    {
        ComPtr<ID3D11Resource> resource;
        target.view->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> color;
        if (FAILED(resource.As(&color)) || !color) {
            return make_error(ErrorCode::invalid_argument,
                "D3D11 target view is not a 2D texture");
        }
        D3D11_TEXTURE2D_DESC desc{};
        color->GetDesc(&desc);
        // The view format decides what the pipeline writes: a NULL-desc view
        // reports the texture format, a typeless texture needs a typed view.
        D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
        target.view->GetDesc(&view_desc);
        if (desc.SampleDesc.Count != 1 ||
            view_desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
            view_desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
            return make_error(ErrorCode::unsupported,
                "D3D11 target is multisampled (" +
                std::to_string(desc.SampleDesc.Count) +
                " samples); only single-sample targets are supported");
        }
        if (view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) {
            return make_error(ErrorCode::unsupported,
                "D3D11 target view dimension " +
                std::to_string(static_cast<int>(view_desc.ViewDimension)) +
                " is unsupported; only TEXTURE2D views are accepted");
        }
        encoding = target_encoding(view_desc.Format);
        if (encoding == TargetEncoding::unsupported) {
            return make_error(ErrorCode::unsupported,
                "D3D11 target view format (DXGI_FORMAT " +
                std::to_string(static_cast<int>(view_desc.Format)) +
                ") is unsupported; accepted view formats are "
                "R8G8B8A8_UNORM, B8G8R8A8_UNORM, R8G8B8A8_UNORM_SRGB, "
                "B8G8R8A8_UNORM_SRGB and R10G10B10A2_UNORM");
        }
        const UINT mip = view_desc.Texture2D.MipSlice;
        const UINT mip_w = mip < 32 ? (desc.Width >> mip) : 0;
        const UINT mip_h = mip < 32 ? (desc.Height >> mip) : 0;
        if ((mip_w != 0 ? mip_w : 1) != target.width ||
            (mip_h != 0 ? mip_h : 1) != target.height) {
            return make_error(ErrorCode::invalid_argument,
                "D3D11 target dimensions do not match its texture");
        }
    }

    detail::Saved saved;
    RestoreGuard guard;
    if (policy == StatePolicy::restore) {
        detail::save_state(context, saved);
        guard.context = context;
        guard.saved = &saved;
        guard.active = true;
    }

    auto fail = [&](Error error) -> Status {
        // RestoreGuard restores host state on scope exit.
        return std::move(error);
    };

    ID3D11RenderTargetView* rtv = target.view;
    context->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT viewport{0, 0,
        static_cast<FLOAT>(target.width), static_cast<FLOAT>(target.height),
        0, 1};
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(impl_->layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(impl_->vertex_shader.Get(), nullptr, 0);
    // _SRGB views decode the encoded output so the hardware re-encode stores
    // the UNORM path's values; UNORM views keep the encoded output as is.
    ID3D11PixelShader* pixel_shader = encoding == TargetEncoding::srgb
        ? impl_->pixel_shader_srgb.Get() : impl_->pixel_shader.Get();
    context->PSSetShader(pixel_shader, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetSamplers(0, 1, impl_->sampler.GetAddressOf());
    constexpr FLOAT blend_factor[4] = {0, 0, 0, 0};
    context->OMSetBlendState(impl_->blend.Get(), blend_factor, 0xffffffff);
    context->RSSetState(impl_->rasterizer.Get());
    context->OMSetDepthStencilState(impl_->depth_stencil.Get(), 0);

    const ViewportConstants viewport_value{
        packet->viewport.x, packet->viewport.y,
        packet->viewport.w, packet->viewport.h};
    if (!write_dynamic(context, impl_->viewport_cbuf.Get(), viewport_value)) {
        return fail(hr_error(E_FAIL, "Map viewport constants failed"));
    }
    ID3D11Buffer* vs_cbuf = impl_->viewport_cbuf.Get();
    context->VSSetConstantBuffers(0, 1, &vs_cbuf);

    const std::uint64_t vertex_bytes =
        static_cast<std::uint64_t>(packet->vertices.size()) * sizeof(Vertex);
    const std::uint64_t index_bytes =
        static_cast<std::uint64_t>(packet->indices.size()) * sizeof(std::uint32_t);
    if (vertex_bytes > std::numeric_limits<UINT>::max() ||
        index_bytes > std::numeric_limits<UINT>::max()) {
        return fail(make_error(ErrorCode::invalid_packet,
            "Packet buffer byte size exceeds API range"));
    }
    if (const auto error = ensure_buffer(device, impl_->vertex_buffer,
            impl_->vertex_capacity, static_cast<std::size_t>(vertex_bytes),
            D3D11_BIND_VERTEX_BUFFER)) {
        return fail(std::move(*error));
    }
    if (const auto error = ensure_buffer(device, impl_->index_buffer,
            impl_->index_capacity, static_cast<std::size_t>(index_bytes),
            D3D11_BIND_INDEX_BUFFER)) {
        return fail(std::move(*error));
    }
    if (!packet->vertices.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(impl_->vertex_buffer.Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return fail(hr_error(E_FAIL, "Map vertex buffer failed"));
        }
        std::memcpy(mapped.pData, packet->vertices.data(),
            static_cast<std::size_t>(vertex_bytes));
        context->Unmap(impl_->vertex_buffer.Get(), 0);
    }
    if (!packet->indices.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(impl_->index_buffer.Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return fail(hr_error(E_FAIL, "Map index buffer failed"));
        }
        std::memcpy(mapped.pData, packet->indices.data(),
            static_cast<std::size_t>(index_bytes));
        context->Unmap(impl_->index_buffer.Get(), 0);
    }
    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    context->IASetVertexBuffers(
        0, 1, impl_->vertex_buffer.GetAddressOf(), &stride, &offset);
    context->IASetIndexBuffer(
        impl_->index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    for (const auto& snapshot : packet->textures) {
        if (!snapshot) {
            return fail(make_error(ErrorCode::invalid_packet,
                "Packet holds a null texture snapshot"));
        }
        if (const auto error = upload_texture(device, impl_->textures, snapshot)) {
            return fail(std::move(*error));
        }
    }

    for (const auto& batch : packet->batches) {
        if (batch.texture_index >= packet->textures.size()) {
            return fail(make_error(ErrorCode::invalid_packet,
                "Invalid texture index"));
        }
        const auto& snapshot = packet->textures[batch.texture_index];
        const TextureEntry* bound = find_texture(impl_->textures, *snapshot);
        if (bound == nullptr) {
            return fail(make_error(ErrorCode::backend_failure,
                "Missing uploaded texture revision"));
        }
        const auto& entry = *bound;
        const auto pixels = ::wui::detail::physical_scissor(batch.clip, packet->viewport,
            packet->scale, target.width, target.height);
        if (pixels.empty()) continue;
        const D3D11_RECT scissor{pixels.left, pixels.top, pixels.right, pixels.bottom};
        context->RSSetScissorRects(1, &scissor);
        ID3D11ShaderResourceView* srv = entry.srv.Get();
        context->PSSetShaderResources(0, 1, &srv);
        const ModeConstants mode{entry.alpha == AlphaMode::straight ? 1 : 0};
        if (!write_dynamic(context, impl_->mode_cbuf.Get(), mode)) {
            return fail(hr_error(E_FAIL, "Map texture-mode constants failed"));
        }
        ID3D11Buffer* ps_cbuf = impl_->mode_cbuf.Get();
        context->PSSetConstantBuffers(1, 1, &ps_cbuf);
        context->DrawIndexed(batch.index_count, batch.first_index,
            batch.base_vertex);
    }
    ID3D11ShaderResourceView* null_srv = nullptr;
    context->PSSetShaderResources(0, 1, &null_srv);

    D3D11_QUERY_DESC query_desc{};
    query_desc.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> query;
    if (FAILED(device->CreateQuery(&query_desc, query.GetAddressOf()))) {
        return fail(hr_error(E_FAIL, "CreateQuery failed"));
    }
    context->End(query.Get());
    impl_->in_flight.push_back(Submission{packet, std::move(query)});
    return std::nullopt;
}

Status Renderer::collect(ID3D11DeviceContext* context) {
    if (!impl_) {
        return make_error(ErrorCode::invalid_frame,
            "D3D11 renderer is moved or uninitialized");
    }
    if (context == nullptr) {
        return make_error(ErrorCode::invalid_argument,
            "D3D11 collect requires a device context");
    }
    if (FAILED(impl_->device->GetDeviceRemovedReason())) {
        return make_error(ErrorCode::device_lost, "D3D11 device removed");
    }
    while (!impl_->in_flight.empty()) {
        const HRESULT hr = context->GetData(
            impl_->in_flight.front().query.Get(), nullptr, 0,
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            impl_->in_flight.erase(impl_->in_flight.begin());
        } else if (hr == S_FALSE) {
            break;
        } else {
            return hr_error(hr, "D3D11 query poll failed");
        }
    }
    auto referenced_by_in_flight = [&](const TextureEntry& entry) {
        for (const auto& submission : impl_->in_flight) {
            if (!submission.packet) continue;
            for (const auto& snapshot : submission.packet->textures) {
                if (!snapshot) continue;
                if (same_texture(entry, *snapshot)) return true;
            }
        }
        return false;
    };
    for (auto it = impl_->textures.begin(); it != impl_->textures.end();) {
        if (it->source.expired() && !referenced_by_in_flight(*it)) {
            it = impl_->textures.erase(it);
        } else {
            ++it;
        }
    }
    return std::nullopt;
}

Status Renderer::recreate(ID3D11Device* replacement_device) {
    if (!impl_) {
        return make_error(ErrorCode::invalid_frame,
            "D3D11 renderer is moved or uninitialized");
    }
    if (replacement_device == nullptr) {
        return make_error(ErrorCode::invalid_argument,
            "D3D11 recreate requires a replacement device");
    }
    // The caller stopped using the old device: drop every UI-owned resource
    // tied to it, including pending submissions, then rebuild lazily from
    // retained CPU snapshots on the next render.
    impl_->vertex_shader.Reset();
    impl_->pixel_shader.Reset();
    impl_->pixel_shader_srgb.Reset();
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
    impl_->device = replacement_device;
    if (replacement_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        return make_error(ErrorCode::unsupported,
            "D3D11 renderer requires feature level 11_0");
    }
    return init_device_objects(replacement_device, *impl_);
}

} // namespace wui::d3d11
