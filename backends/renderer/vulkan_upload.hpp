#pragma once

#include <wui/backends/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wui::vulkan::detail {

// Native-only test view; no graphics handles are exposed through draw packets.
struct TextureVersion {
    VkImage image = VK_NULL_HANDLE;
    std::uint64_t revision = 0;
};
struct SlotView {
    std::size_t buffers = 0;
    bool noncoherent_staging = false;
    std::size_t noncoherent = 0;   // upload buffers in non-host-coherent memory
    std::size_t flushed = 0;       // upload buffers flushed with vkFlushMappedMemoryRanges
    std::vector<TextureVersion> textures;
};
struct Inspection {
    static SlotView slot(const Renderer& renderer, std::uint32_t index);
    static PFN_vkGetFenceStatus replace_fence_status(
        Renderer& renderer, PFN_vkGetFenceStatus replacement) noexcept;
};

} // namespace wui::vulkan::detail
