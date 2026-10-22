#pragma once

#ifndef _WIN32
#error "wui::d3d11 requires Windows"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wui/draw.hpp>
#include <cstdint>
#include <memory>

namespace wui::d3d11 {

// Isolated D3D11 renderer consuming immutable DrawPackets. The caller owns
// the device, context, render target and presentation. The renderer never
// creates windows, swap chains or worker threads. All submission happens on
// the caller's thread with external synchronization.
enum class StatePolicy { restore, caller_restores };

// The view must be a single-sample TEXTURE2D render-target view; width and
// height must equal the size of its mip slice and round(viewport * scale) of
// the packet. The view format (not the texture format, which may be
// typeless) must be R8G8B8A8_UNORM, B8G8R8A8_UNORM or R10G10B10A2_UNORM
// (encoded SDR values are stored as is), or R8G8B8A8_UNORM_SRGB or
// B8G8R8A8_UNORM_SRGB (output is decoded to linear before the hardware
// re-encode, so opaque pixels store the same values as on UNORM; translucent
// edges blend in linear light). Any other format, notably
// R16G16B16A16_FLOAT scRGB/HDR, and MSAA or array views return `unsupported`
// before any device state changes.
struct Target {
    ID3D11RenderTargetView* view = nullptr;
    std::uint32_t width = 0, height = 0;
};

class Renderer {
public:
    static Result<std::unique_ptr<Renderer>> create(ID3D11Device* device);
    Status render(ID3D11DeviceContext* context, const DrawPacket& packet,
        Target target, StatePolicy policy = StatePolicy::restore);
    Status collect(ID3D11DeviceContext* context);
    Status recreate(ID3D11Device* replacement_device);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

private:
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static Status init_device_objects(ID3D11Device* device, Impl& impl);
};

} // namespace wui::d3d11
