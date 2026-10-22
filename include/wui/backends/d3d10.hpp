#pragma once

#ifndef _WIN32
#error "wui::d3d10 requires Windows"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d10.h>
#include <wui/draw.hpp>
#include <cstdint>
#include <memory>

namespace wui::d3d10 {

// Isolated D3D10 renderer consuming immutable DrawPackets. The caller owns
// the device, render target and presentation; ID3D10Device is also the
// immediate context. The renderer never creates windows, swap chains or
// worker threads. All submission happens on the caller's thread with
// external synchronization.
enum class StatePolicy { restore, caller_restores };

struct Target {
    ID3D10RenderTargetView* view = nullptr;
    std::uint32_t width = 0, height = 0;
};

class Renderer {
public:
    // Requires a feature level 10_0-class device. When the device exposes
    // ID3D10Device1, its feature level must be at least 10_0.
    static Result<std::unique_ptr<Renderer>> create(ID3D10Device* device);
    Status render(const DrawPacket& packet, Target target,
        StatePolicy policy = StatePolicy::restore);
    Status collect();
    Status recreate(ID3D10Device* replacement_device);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

private:
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static Status init_device_objects(ID3D10Device* device, Impl& impl);
};

} // namespace wui::d3d10
