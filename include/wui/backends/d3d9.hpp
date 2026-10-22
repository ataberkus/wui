#pragma once

#ifndef _WIN32
#error "wui::d3d9 requires Windows"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <wui/draw.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace wui::d3d9 {

// Caller owns the scene, target and presentation. Rendering is serialized on
// the caller's device thread; this backend only owns its own GPU resources.
enum class StatePolicy { restore, caller_restores };

struct Target {
    IDirect3DSurface9* surface = nullptr;
    std::uint32_t width = 0, height = 0;
};

struct Chunk {
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    std::uint32_t texture_index = 0;
    Rect clip;
};

Result<std::vector<Chunk>> make_chunks(const PacketData& packet,
    std::uint32_t max_vertices, std::uint32_t max_primitives);

class Renderer {
public:
    static Result<std::unique_ptr<Renderer>> create(IDirect3DDevice9* device);
    Status render(const DrawPacket& packet, Target target,
        StatePolicy policy = StatePolicy::restore);
    Status collect();
    // Host quiesces use or enters lost-device recovery before this call.
    void before_reset();
    Status after_reset();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

private:
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wui::d3d9
