#pragma once

#ifndef _WIN32
#error "wui::d3d12 requires Windows"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wui/draw.hpp>

#include <cstdint>
#include <memory>

namespace wui::d3d12 {

// The host owns the device, command queue, command allocators and lists,
// fences, render targets, submission and presentation. The renderer records
// into caller-supplied command lists and never executes, signals or waits.
struct Config {
    ID3D12Device* device = nullptr;
    DXGI_FORMAT rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::uint32_t sample_count = 1;
    std::uint32_t max_frames = 3;     // frames in flight the host will use (1..64)
    std::uint32_t max_textures = 256; // shader-visible SRV capacity per frame slot
};

// `fence` reaches `completion_value` once the host's submission containing
// this frame's command lists has finished on the GPU.
struct FrameSlot {
    std::uint32_t index = 0;
    ID3D12Fence* fence = nullptr;
    std::uint64_t completion_value = 0;
};

// The render target must already be in D3D12_RESOURCE_STATE_RENDER_TARGET.
struct Target {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    std::uint32_t width = 0, height = 0;
};

class Renderer;
namespace detail { struct Inspection; }

// Move-only frame ticket. Destroying an unsubmitted ticket abandons it: the
// host must have discarded its command lists unexecuted before that point.
class PreparedFrame {
public:
    PreparedFrame(PreparedFrame&& other) noexcept;
    PreparedFrame& operator=(PreparedFrame&& other) noexcept;
    PreparedFrame(const PreparedFrame&) = delete;
    PreparedFrame& operator=(const PreparedFrame&) = delete;
    ~PreparedFrame();

private:
    friend class Renderer;
    PreparedFrame(Renderer* owner, std::uint32_t index) noexcept;
    Renderer* owner_;
    std::uint32_t index_;
};

class Renderer {
public:
    static Result<std::unique_ptr<Renderer>> create(Config config);
    // Records texture uploads (upload ring + CopyTextureRegion + barriers
    // COPY_DEST -> PIXEL_SHADER_RESOURCE) into `list` and copies vertices and
    // indices into the slot's upload-heap buffers. The host must execute
    // `list` before, or in the same submission as, the list passed to record().
    Result<PreparedFrame> prepare(FrameSlot slot, ID3D12GraphicsCommandList* list,
        const DrawPacket& packet);
    // Sets the root signature, pipeline, the renderer's own shader-visible
    // descriptor heap, render target, viewport and scissor, then draws in
    // packet order. The host rebinds its own state, including descriptor
    // heaps, afterward.
    Status record(const PreparedFrame& prepared, ID3D12GraphicsCommandList* list, Target target);
    void submitted(PreparedFrame&& prepared) noexcept; // host signalled slot.fence to completion_value
    void abandon(PreparedFrame&& prepared) noexcept;   // host discarded the lists unexecuted
    Status collect();                                  // nonblocking GetCompletedValue polling
    Status shutdown();                                 // requires all submitted fences complete
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

private:
    friend struct detail::Inspection;
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wui::d3d12
