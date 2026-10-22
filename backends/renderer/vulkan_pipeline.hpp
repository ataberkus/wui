#pragma once

#include <wui/backends/vulkan.hpp>

#include <cstddef>
#include <cstdint>

namespace wui::vulkan::detail {

struct PushConstants {
    float viewport[4];
    std::uint32_t texture_is_straight;
};
static_assert(sizeof(PushConstants) == 20);
static_assert(offsetof(PushConstants, texture_is_straight) == 16);

struct Pipeline {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline graphics = VK_NULL_HANDLE;
};

Status create_pipeline(const Config& config, VkDescriptorSetLayout texture_layout,
    Pipeline& output);
void destroy_pipeline(VkDevice device, Pipeline& pipeline) noexcept;

} // namespace wui::vulkan::detail
