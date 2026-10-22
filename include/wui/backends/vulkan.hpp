#pragma once

#include <vulkan/vulkan.h>
#include <wui/draw.hpp>

#include <cstdint>
#include <memory>

namespace wui::vulkan {

// The host owns the device, render pass, command buffers, fences and submission.
struct Config {
    VkPhysicalDevice physical_device;
    VkDevice device;
    std::uint32_t graphics_family;
    VkRenderPass render_pass;
    std::uint32_t subpass = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::uint32_t max_frames = 3;
    // Test-only: flush every mapped upload with nonCoherentAtomSize-aligned
    // ranges even on host-coherent memory, so the noncoherent path runs under
    // the validation layer. Off in production; noncoherent memory always flushes.
    bool force_explicit_flush = false;
};

struct FrameSlot {
    std::uint32_t index;
    VkCommandBuffer commands;
    VkFence completion;
};

// `render_pass` may differ from Config::render_pass when it is render-pass
// compatible with it (same attachment formats, sample counts and subpass
// structure). The host declares the color attachment's format and sample count;
// a mismatch with the renderer's pipeline is rejected with ErrorCode::unsupported.
struct Target {
    VkRenderPass render_pass;
    std::uint32_t subpass;
    VkExtent2D extent;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

class Renderer;
namespace detail { struct Inspection; }

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
    Result<PreparedFrame> prepare(FrameSlot slot, const DrawPacket& packet);
    Status record(const PreparedFrame& prepared, Target target);
    void submitted(PreparedFrame&& prepared) noexcept;
    void abandon(PreparedFrame&& prepared) noexcept;
    Status collect();
    Status shutdown();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

private:
    friend struct detail::Inspection;
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wui::vulkan
