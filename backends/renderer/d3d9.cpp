#include "d3d9_caps.hpp"
#include "scissor.hpp"
#include "d3d9_state.hpp"
#include "wui_d3d9_shader_source.hpp"

#include <d3dcompiler.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace wui::d3d9 {
namespace {

Status unsupported(const char* capability) {
    return Error{ErrorCode::unsupported,
        std::string("D3D9 requires ") + capability};
}

} // namespace

namespace detail {

Status validate_caps(const D3DCAPS9& caps) {
    if (caps.DeviceType != D3DDEVTYPE_HAL) return unsupported("HAL device");
    if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
        return unsupported("VertexShaderVersion >= 3.0");
    }
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0)) {
        return unsupported("PixelShaderVersion >= 3.0");
    }
    if (caps.MaxVertexIndex < 2) return unsupported("MaxVertexIndex >= 2");
    if (caps.MaxPrimitiveCount == 0) return unsupported("MaxPrimitiveCount >= 1");
    if ((caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST) == 0) {
        return unsupported("RasterCaps SCISSORTEST");
    }
    if ((caps.SrcBlendCaps & D3DPBLENDCAPS_ONE) == 0) {
        return unsupported("SrcBlendCaps ONE");
    }
    if ((caps.DestBlendCaps & D3DPBLENDCAPS_INVSRCALPHA) == 0) {
        return unsupported("DestBlendCaps INVSRCALPHA");
    }
    if ((caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) == 0) {
        return unsupported("PrimitiveMiscCaps SEPARATEALPHABLEND");
    }
    if (caps.MaxTextureWidth < 4) return unsupported("MaxTextureWidth >= 4");
    if (caps.MaxTextureHeight < 4) return unsupported("MaxTextureHeight >= 4");
    if ((caps.TextureCaps & D3DPTEXTURECAPS_ALPHA) == 0) {
        return unsupported("TEXTURECAPS_ALPHA");
    }
    if ((caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) != 0) {
        return unsupported("TEXTURECAPS without SQUAREONLY");
    }
    if ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0 &&
        (caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) == 0) {
        return unsupported("TEXTURECAPS NONPOW2CONDITIONAL or unrestricted NPOT");
    }
    if ((caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) == 0) {
        return unsupported("TextureFilterCaps MINFLINEAR");
    }
    if ((caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) == 0) {
        return unsupported("TextureFilterCaps MAGFLINEAR");
    }
    return std::nullopt;
}

} // namespace detail

using Microsoft::WRL::ComPtr;

namespace {

constexpr std::size_t kMaxInFlight = 3;

Error api_error(HRESULT hr, const char* operation) {
    char code[32] = {};
    std::snprintf(code, sizeof(code), " (hr=0x%08lX)", static_cast<unsigned long>(hr));
    return Error{ErrorCode::backend_failure, std::string(operation) + code};
}

Status compile_shader(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> diagnostics;
    // Generic SM3 sampler syntax keeps strict HLSL validation enabled.
    const HRESULT hr = D3DCompile(detail::kShaderSource,
        sizeof(detail::kShaderSource) - 1, "wui_d3d9", nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &out, &diagnostics);
    if (SUCCEEDED(hr) && out) return std::nullopt;
    std::string message = std::string("D3D9 ") + target + " shader compilation failed";
    if (diagnostics && diagnostics->GetBufferPointer()) {
        message += ": ";
        message.append(static_cast<const char*>(diagnostics->GetBufferPointer()),
            diagnostics->GetBufferSize());
    }
    return Error{ErrorCode::backend_failure, std::move(message)};
}

struct TextureEntry {
    TextureHandle handle;
    std::uint64_t revision = 0;
    std::uint32_t width = 0, height = 0;
    AlphaMode alpha = AlphaMode::straight;
    std::weak_ptr<const TextureSnapshot> source;
    ComPtr<IDirect3DTexture9> texture;
};

Status texture_for(IDirect3DDevice9* device, const D3DCAPS9& caps,
    std::vector<TextureEntry>& entries,
    const std::shared_ptr<const TextureSnapshot>& snapshot,
    const TextureEntry*& result) {
    const auto& source = *snapshot;
    if (source.width > caps.MaxTextureWidth || source.height > caps.MaxTextureHeight) {
        return Error{ErrorCode::unsupported, "D3D9 MaxTextureWidth/MaxTextureHeight exceeded"};
    }
    for (auto& entry : entries) {
        if (entry.handle == source.handle && entry.revision == source.revision &&
            entry.width == source.width && entry.height == source.height &&
            entry.alpha == source.alpha && entry.source.lock() == snapshot) {
            result = &entry;
            return std::nullopt;
        }
    }
    TextureEntry entry;
    entry.handle = source.handle;
    entry.revision = source.revision;
    entry.width = source.width;
    entry.height = source.height;
    entry.alpha = source.alpha;
    entry.source = snapshot;
    HRESULT hr = device->CreateTexture(source.width, source.height, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &entry.texture, nullptr);
    if (FAILED(hr)) return api_error(hr, "D3D9 CreateTexture(A8R8G8B8) failed");
    D3DLOCKED_RECT locked{};
    hr = entry.texture->LockRect(0, &locked, nullptr, 0);
    if (FAILED(hr)) return api_error(hr, "D3D9 texture LockRect failed");
    const auto stride = static_cast<std::size_t>(source.width) * 4;
    if (locked.Pitch < 0 || static_cast<std::size_t>(locked.Pitch) < stride) {
        entry.texture->UnlockRect(0);
        return Error{ErrorCode::backend_failure, "D3D9 texture pitch is too small"};
    }
    for (std::uint32_t y = 0; y < source.height; ++y) {
        auto* dst = static_cast<std::uint8_t*>(locked.pBits) +
            static_cast<std::size_t>(y) * locked.Pitch;
        const auto* src = reinterpret_cast<const std::uint8_t*>(source.rgba.data()) +
            static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < source.width; ++x) {
            dst[4 * x] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x];
            dst[4 * x + 3] = src[4 * x + 3];
        }
    }
    hr = entry.texture->UnlockRect(0);
    if (FAILED(hr)) return api_error(hr, "D3D9 texture UnlockRect failed");
    entries.push_back(std::move(entry));
    result = &entries.back();
    return std::nullopt;
}

struct RestoreGuard {
    IDirect3DDevice9* device = nullptr;
    const detail::Saved* saved = nullptr;
    ~RestoreGuard() { if (saved) detail::restore_state(device, *saved); }
};

struct Submission {
    DrawPacket packet;
    ComPtr<IDirect3DQuery9> query;
    std::vector<ComPtr<IDirect3DTexture9>> textures;
};

} // namespace

struct Renderer::Impl {
    ComPtr<IDirect3DDevice9> device;
    D3DCAPS9 caps{};
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    ComPtr<IDirect3DVertexShader9> vertex_shader;
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    ComPtr<IDirect3DVertexBuffer9> vertices;
    ComPtr<IDirect3DIndexBuffer9> indices;
    UINT vertex_capacity = 0, index_capacity = 0;
    std::vector<TextureEntry> textures;
    std::vector<Submission> in_flight;
    bool reset_pending = false, query_failed = false;

    struct Pending {
        Impl& owner;
        Submission submission;
        bool drew = false, finished = false;

        ~Pending() {
            if (drew && !finished) (void)finish(); // Also retires partial/error draws.
        }
        void use(IDirect3DTexture9* texture) {
            for (const auto& held : submission.textures) {
                if (held.Get() == texture) return;
            }
            submission.textures.emplace_back(texture);
        }
        Status finish() {
            if (finished) return std::nullopt;
            finished = true;
            if (!drew) return std::nullopt;
            const HRESULT hr = submission.query->Issue(D3DISSUE_END);
            owner.in_flight.push_back(std::move(submission));
            if (FAILED(hr)) {
                owner.query_failed = true; // Keep the leases until before_reset.
                if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET) {
                    return Error{ErrorCode::device_lost, "D3D9 Issue(EVENT) device lost"};
                }
                return api_error(hr, "D3D9 Issue(EVENT) failed");
            }
            return std::nullopt;
        }
    };

    Status initialize_shaders() {
        ComPtr<ID3DBlob> vertex, pixel;
        if (const auto error = compile_shader("VsMain", "vs_3_0", vertex)) return error;
        if (const auto error = compile_shader("PsMain", "ps_3_0", pixel)) return error;
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END()
        };
        HRESULT hr = device->CreateVertexDeclaration(elements, &declaration);
        if (FAILED(hr)) return api_error(hr, "D3D9 CreateVertexDeclaration failed");
        hr = device->CreateVertexShader(
            static_cast<const DWORD*>(vertex->GetBufferPointer()), &vertex_shader);
        if (FAILED(hr)) return api_error(hr, "D3D9 CreateVertexShader failed");
        hr = device->CreatePixelShader(
            static_cast<const DWORD*>(pixel->GetBufferPointer()), &pixel_shader);
        if (FAILED(hr)) return api_error(hr, "D3D9 CreatePixelShader failed");
        return std::nullopt;
    }

    Status ensure_buffers(UINT vertex_bytes, UINT index_bytes) {
        if (vertex_bytes > vertex_capacity) {
            ComPtr<IDirect3DVertexBuffer9> replacement;
            const HRESULT hr = device->CreateVertexBuffer(vertex_bytes,
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                &replacement, nullptr);
            if (FAILED(hr)) return api_error(hr, "D3D9 CreateVertexBuffer failed");
            vertices = std::move(replacement);
            vertex_capacity = vertex_bytes;
        }
        if (index_bytes > index_capacity) {
            ComPtr<IDirect3DIndexBuffer9> replacement;
            const HRESULT hr = device->CreateIndexBuffer(index_bytes,
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                D3DPOOL_DEFAULT, &replacement, nullptr);
            if (FAILED(hr)) return api_error(hr, "D3D9 CreateIndexBuffer failed");
            indices = std::move(replacement);
            index_capacity = index_bytes;
        }
        return std::nullopt;
    }
};

Renderer::~Renderer() = default;

Result<std::unique_ptr<Renderer>> Renderer::create(IDirect3DDevice9* device) {
    if (!device) return Error{ErrorCode::invalid_argument, "D3D9 renderer requires a device"};

    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (FAILED(device->GetCreationParameters(&creation))) {
        return Error{ErrorCode::backend_failure, "D3D9 GetCreationParameters failed"};
    }
    if (creation.DeviceType != D3DDEVTYPE_HAL) {
        return *unsupported("HAL device");
    }
    D3DCAPS9 caps{};
    if (FAILED(device->GetDeviceCaps(&caps))) {
        return Error{ErrorCode::backend_failure, "D3D9 GetDeviceCaps failed"};
    }
    if (const auto error = detail::validate_caps(caps)) return *error;

    Microsoft::WRL::ComPtr<IDirect3D9> api;
    if (FAILED(device->GetDirect3D(&api)) || !api) {
        return Error{ErrorCode::backend_failure, "D3D9 GetDirect3D failed"};
    }
    D3DDISPLAYMODE mode{};
    if (FAILED(api->GetAdapterDisplayMode(creation.AdapterOrdinal, &mode))) {
        return Error{ErrorCode::backend_failure, "D3D9 GetAdapterDisplayMode failed"};
    }
    if (FAILED(api->CheckDeviceFormat(creation.AdapterOrdinal, creation.DeviceType,
            mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8))) {
        return *unsupported("CheckDeviceFormat A8R8G8B8 RGBA upload");
    }
    Microsoft::WRL::ComPtr<IDirect3DQuery9> event;
    if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &event)) || !event) {
        return *unsupported("D3DQUERYTYPE_EVENT completion query");
    }

    auto renderer = std::unique_ptr<Renderer>(new Renderer());
    renderer->impl_ = std::make_unique<Impl>();
    renderer->impl_->device = device;
    renderer->impl_->caps = caps;
    if (const auto error = renderer->impl_->initialize_shaders()) return *error;
    renderer->impl_->in_flight.reserve(kMaxInFlight);
    return renderer;
}

Status Renderer::render(const DrawPacket& packet, Target target, StatePolicy policy) {
    if (impl_->reset_pending || impl_->query_failed) {
        return Error{ErrorCode::device_lost, "D3D9 device awaits reset"};
    }
    const HRESULT cooperative = impl_->device->TestCooperativeLevel();
    if (cooperative == D3DERR_DEVICELOST || cooperative == D3DERR_DEVICENOTRESET) {
        return Error{ErrorCode::device_lost, "D3D9 device lost"};
    }
    if (FAILED(cooperative)) return api_error(cooperative, "D3D9 TestCooperativeLevel failed");
    if (impl_->in_flight.size() >= kMaxInFlight) {
        return Error{ErrorCode::busy, "D3D9 renderer has 3 frames in flight"};
    }
    if (!packet) return Error{ErrorCode::invalid_argument, "D3D9 render requires a packet"};
    if (!target.surface || !target.width || !target.height ||
        target.width > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()) ||
        target.height > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())) {
        return Error{ErrorCode::invalid_argument, "D3D9 render requires a valid target"};
    }
    const auto max_vertices = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(65535, std::uint64_t{impl_->caps.MaxVertexIndex} + 1));
    auto converted = make_chunks(*packet, max_vertices, impl_->caps.MaxPrimitiveCount);
    if (auto* error = std::get_if<Error>(&converted)) return std::move(*error);
    auto chunks = std::get<std::vector<Chunk>>(std::move(converted));
    const double width = static_cast<double>(packet->viewport.w) * packet->scale;
    const double height = static_cast<double>(packet->viewport.h) * packet->scale;
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width < 0 || height < 0 ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max() ||
        std::llround(width) != target.width || std::llround(height) != target.height) {
        return Error{ErrorCode::invalid_argument,
            "Packet viewport and scale do not match D3D9 target dimensions"};
    }

    auto* device = impl_->device.Get();
    D3DSURFACE_DESC description{};
    HRESULT hr = target.surface->GetDesc(&description);
    if (FAILED(hr)) return api_error(hr, "D3D9 target GetDesc failed");
    if (description.Width != target.width || description.Height != target.height) {
        return Error{ErrorCode::invalid_argument, "D3D9 target surface dimensions mismatch"};
    }
    if ((description.Format != D3DFMT_A8R8G8B8 &&
         description.Format != D3DFMT_X8R8G8B8) ||
        description.MultiSampleType != D3DMULTISAMPLE_NONE ||
        (description.Usage & D3DUSAGE_RENDERTARGET) == 0) {
        return Error{ErrorCode::unsupported,
            "D3D9 target requires single-sample A8R8G8B8 or X8R8G8B8 render surface"};
    }
    ComPtr<IDirect3DDevice9> target_device;
    hr = target.surface->GetDevice(&target_device);
    if (FAILED(hr)) return api_error(hr, "D3D9 target GetDevice failed");
    if (target_device.Get() != device) {
        return Error{ErrorCode::invalid_argument, "D3D9 target belongs to another device"};
    }
    if (chunks.empty()) return std::nullopt;
    Impl::Pending pending{*impl_};
    pending.submission.packet = packet;
    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &pending.submission.query);
    if (FAILED(hr)) {
        if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET) {
            return Error{ErrorCode::device_lost, "D3D9 CreateQuery(EVENT) device lost"};
        }
        return api_error(hr, "D3D9 CreateQuery(EVENT) failed");
    }
    try {
        pending.submission.textures.reserve(
            std::min(packet->textures.size(), chunks.size()));
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, "D3D9 submission allocation failed"};
    }

    detail::Saved saved;
    RestoreGuard guard;
    if (policy == StatePolicy::restore) {
        if (const auto error = detail::save_state(device,
                impl_->caps.NumSimultaneousRTs, saved)) return error;
        guard.device = device;
        guard.saved = &saved;
    }
    const auto checked = [](HRESULT result, const char* operation) -> Status {
        if (FAILED(result)) return api_error(result, operation);
        return std::nullopt;
    };
    if (const auto error = checked(device->SetDepthStencilSurface(nullptr),
            "D3D9 unbind depth failed")) return error;
    if (const auto error = checked(device->SetRenderTarget(0, target.surface),
            "D3D9 SetRenderTarget failed")) return error;
    for (UINT i = 1; i < std::min<UINT>(impl_->caps.NumSimultaneousRTs, 4); ++i) {
        if (const auto error = checked(device->SetRenderTarget(i, nullptr),
                "D3D9 unbind extra render target failed")) return error;
    }
    const D3DVIEWPORT9 viewport{0, 0, target.width, target.height, 0, 1};
    if (const auto error = checked(device->SetViewport(&viewport),
            "D3D9 SetViewport failed")) return error;
    const struct { D3DRENDERSTATETYPE state; DWORD value; } render_states[] = {
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE},
        {D3DRS_STENCILENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_FOGENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE}, {D3DRS_SCISSORTESTENABLE, TRUE},
        {D3DRS_ALPHABLENDENABLE, TRUE}, {D3DRS_SRCBLEND, D3DBLEND_ONE},
        {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, TRUE},
        {D3DRS_SRCBLENDALPHA, D3DBLEND_ONE},
        {D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA},
        {D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD},
        {D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED |
            D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
            D3DCOLORWRITEENABLE_ALPHA},
        {D3DRS_SRGBWRITEENABLE, FALSE}
    };
    for (const auto& setting : render_states) {
        if (const auto error = checked(device->SetRenderState(setting.state, setting.value),
                "D3D9 SetRenderState failed")) return error;
    }
    const struct { D3DSAMPLERSTATETYPE state; DWORD value; } sampler_states[] = {
        {D3DSAMP_MINFILTER, D3DTEXF_LINEAR}, {D3DSAMP_MAGFILTER, D3DTEXF_LINEAR},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE},
        {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
        {D3DSAMP_SRGBTEXTURE, FALSE}
    };
    for (const auto& setting : sampler_states) {
        if (const auto error = checked(device->SetSamplerState(0,
                setting.state, setting.value), "D3D9 SetSamplerState failed")) return error;
    }
    if (const auto error = checked(device->SetVertexDeclaration(impl_->declaration.Get()),
            "D3D9 SetVertexDeclaration failed")) return error;
    if (const auto error = checked(device->SetVertexShader(impl_->vertex_shader.Get()),
            "D3D9 SetVertexShader failed")) return error;
    if (const auto error = checked(device->SetPixelShader(impl_->pixel_shader.Get()),
            "D3D9 SetPixelShader failed")) return error;
    const float logical[4] = {packet->viewport.x, packet->viewport.y,
        packet->viewport.w, packet->viewport.h};
    const float physical[4] = {static_cast<float>(target.width),
        static_cast<float>(target.height), 0, 0};
    if (const auto error = checked(device->SetVertexShaderConstantF(0, logical, 1),
            "D3D9 SetVertexShaderConstantF viewport failed")) return error;
    if (const auto error = checked(device->SetVertexShaderConstantF(1, physical, 1),
            "D3D9 SetVertexShaderConstantF framebuffer failed")) return error;

    try {
        for (const auto& chunk : chunks) {
            const auto pixels = ::wui::detail::physical_scissor(chunk.clip, packet->viewport,
                packet->scale, target.width, target.height);
            if (pixels.empty()) continue;
            const RECT scissor{pixels.left, pixels.top, pixels.right, pixels.bottom};
            if (const auto error = checked(device->SetScissorRect(&scissor),
                    "D3D9 SetScissorRect failed")) return error;

            const auto& snapshot = packet->textures[chunk.texture_index];
            const TextureEntry* texture = nullptr;
            if (const auto error = texture_for(device, impl_->caps, impl_->textures,
                    snapshot, texture)) return error;
            pending.use(texture->texture.Get());
            if (const auto error = checked(device->SetTexture(0, texture->texture.Get()),
                    "D3D9 SetTexture failed")) return error;
            const float mode[4] = {texture->alpha == AlphaMode::straight ? 1.0f : 0.0f,
                0, 0, 0};
            if (const auto error = checked(device->SetPixelShaderConstantF(2, mode, 1),
                    "D3D9 SetPixelShaderConstantF alpha mode failed")) return error;

            const UINT vertex_bytes = static_cast<UINT>(chunk.vertices.size() * sizeof(Vertex));
            const UINT index_bytes = static_cast<UINT>(chunk.indices.size() *
                sizeof(std::uint16_t));
            if (const auto error = impl_->ensure_buffers(vertex_bytes, index_bytes)) return error;
            void* mapped = nullptr;
            hr = impl_->vertices->Lock(0, vertex_bytes, &mapped, D3DLOCK_DISCARD);
            if (FAILED(hr)) return api_error(hr, "D3D9 vertex buffer Lock failed");
            std::memcpy(mapped, chunk.vertices.data(), vertex_bytes);
            hr = impl_->vertices->Unlock();
            if (FAILED(hr)) return api_error(hr, "D3D9 vertex buffer Unlock failed");
            hr = impl_->indices->Lock(0, index_bytes, &mapped, D3DLOCK_DISCARD);
            if (FAILED(hr)) return api_error(hr, "D3D9 index buffer Lock failed");
            std::memcpy(mapped, chunk.indices.data(), index_bytes);
            hr = impl_->indices->Unlock();
            if (FAILED(hr)) return api_error(hr, "D3D9 index buffer Unlock failed");
            if (const auto error = checked(device->SetStreamSource(0,
                    impl_->vertices.Get(), 0, sizeof(Vertex)),
                    "D3D9 SetStreamSource failed")) return error;
            if (const auto error = checked(device->SetIndices(impl_->indices.Get()),
                    "D3D9 SetIndices failed")) return error;
            pending.drew = true;
            if (const auto error = checked(device->DrawIndexedPrimitive(
                    D3DPT_TRIANGLELIST, 0, 0, static_cast<UINT>(chunk.vertices.size()),
                    0, static_cast<UINT>(chunk.indices.size() / 3)),
                    "D3D9 DrawIndexedPrimitive failed")) return error;
        }
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, "D3D9 texture allocation failed"};
    }
    return pending.finish();
}

Status Renderer::collect() {
    if (impl_->reset_pending || impl_->query_failed) {
        return Error{ErrorCode::device_lost, "D3D9 device awaits reset"};
    }
    const HRESULT cooperative = impl_->device->TestCooperativeLevel();
    if (cooperative == D3DERR_DEVICELOST || cooperative == D3DERR_DEVICENOTRESET) {
        return Error{ErrorCode::device_lost, "D3D9 device lost"};
    }
    if (FAILED(cooperative)) return api_error(cooperative, "D3D9 TestCooperativeLevel failed");

    while (!impl_->in_flight.empty()) {
        // Flags 0: never D3DGETDATA_FLUSH and never spin waiting on the GPU.
        const HRESULT hr = impl_->in_flight.front().query->GetData(nullptr, 0, 0);
        if (hr == S_FALSE) break;
        if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET) {
            impl_->query_failed = true;
            return Error{ErrorCode::device_lost, "D3D9 GetData(EVENT) device lost"};
        }
        if (FAILED(hr)) {
            impl_->query_failed = true;
            return api_error(hr, "D3D9 GetData(EVENT) failed");
        }
        impl_->in_flight.erase(impl_->in_flight.begin());
    }
    std::erase_if(impl_->textures, [](const TextureEntry& entry) {
        return entry.source.expired();
    });
    if (impl_->in_flight.size() >= kMaxInFlight) {
        return Error{ErrorCode::busy, "D3D9 renderer has 3 frames in flight"};
    }
    return std::nullopt;
}

void Renderer::before_reset() {
    if (impl_->reset_pending) return;
    impl_->reset_pending = true;
    // The host quiesced submissions or entered lost-device recovery.
    impl_->in_flight.clear();
    auto* device = impl_->device.Get();
    // Reset invalidates every DEFAULT-pool stream/index binding, including an
    // older UI buffer restored by a caller after this renderer grew its buffers.
    device->SetStreamSource(0, nullptr, 0, 0);
    device->SetIndices(nullptr);
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    if (SUCCEEDED(device->GetVertexDeclaration(&declaration)) &&
        declaration.Get() == impl_->declaration.Get()) device->SetVertexDeclaration(nullptr);
    ComPtr<IDirect3DVertexShader9> vertex_shader;
    if (SUCCEEDED(device->GetVertexShader(&vertex_shader)) &&
        vertex_shader.Get() == impl_->vertex_shader.Get()) device->SetVertexShader(nullptr);
    ComPtr<IDirect3DPixelShader9> pixel_shader;
    if (SUCCEEDED(device->GetPixelShader(&pixel_shader)) &&
        pixel_shader.Get() == impl_->pixel_shader.Get()) device->SetPixelShader(nullptr);
    ComPtr<IDirect3DBaseTexture9> bound;
    if (SUCCEEDED(device->GetTexture(0, &bound))) {
        for (const auto& entry : impl_->textures) {
            if (bound.Get() == entry.texture.Get()) {
                device->SetTexture(0, nullptr);
                break;
            }
        }
    }
    bound.Reset();
    pixel_shader.Reset();
    vertex_shader.Reset();
    declaration.Reset();
    impl_->indices.Reset();
    impl_->vertices.Reset();
    impl_->index_capacity = impl_->vertex_capacity = 0;
    impl_->pixel_shader.Reset();
    impl_->vertex_shader.Reset();
    impl_->declaration.Reset();
    impl_->textures.clear();
}

Status Renderer::after_reset() {
    if (!impl_->reset_pending) {
        return Error{ErrorCode::invalid_frame, "D3D9 after_reset requires before_reset"};
    }
    const HRESULT cooperative = impl_->device->TestCooperativeLevel();
    if (cooperative == D3DERR_DEVICELOST || cooperative == D3DERR_DEVICENOTRESET) {
        return Error{ErrorCode::device_lost, "D3D9 Reset has not completed"};
    }
    if (FAILED(cooperative)) return api_error(cooperative, "D3D9 TestCooperativeLevel failed");
    impl_->declaration.Reset();
    impl_->vertex_shader.Reset();
    impl_->pixel_shader.Reset();
    if (const auto error = impl_->initialize_shaders()) return error;
    impl_->query_failed = false;
    impl_->reset_pending = false;
    return std::nullopt;
}

} // namespace wui::d3d9
