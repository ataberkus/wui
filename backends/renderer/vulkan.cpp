#include <wui/backends/vulkan.hpp>

#include "vulkan_memory.hpp"
#include "vulkan_upload.hpp"
#include "vulkan_pipeline.hpp"
#include "scissor.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace wui::vulkan {
namespace {
Status failure(VkResult result, const char* operation) {
    return Error{result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost :
        ErrorCode::backend_failure, operation};
}
} // namespace

namespace {
struct Uploads {
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool staging = false, coherent = false, flushed = false;
    };
    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        std::uint64_t revision = 0;
        std::uint32_t width = 0, height = 0;
    };
    VkDevice device;
    VkBuffer vertices = VK_NULL_HANDLE;
    VkBuffer indices = VK_NULL_HANDLE;
    std::vector<Buffer> buffers;
    std::vector<Image> images;

    explicit Uploads(VkDevice owned_device) : device(owned_device) {}
    ~Uploads() {
        for (auto& image : images) {
            if (image.view) vkDestroyImageView(device, image.view, nullptr);
            if (image.handle) vkDestroyImage(device, image.handle, nullptr);
            if (image.memory) vkFreeMemory(device, image.memory, nullptr);
        }
        for (auto& buffer : buffers) {
            if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr);
            if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        }
    }
};

std::optional<std::uint32_t> memory_type(const VkPhysicalDeviceMemoryProperties& properties,
    std::uint32_t compatible, VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred) {
    std::uint32_t required_types = 0, preferred_types = 0;
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        required_types |= 1u << i;
        if ((flags & preferred) == preferred) preferred_types |= 1u << i;
    }
    return detail::select_memory_type(compatible, required_types, preferred_types);
}

struct UploadMemory {
    VkPhysicalDeviceMemoryProperties properties{};
    VkDeviceSize atom = 1;            // VkPhysicalDeviceLimits::nonCoherentAtomSize
    bool force_explicit_flush = false;
};

Status buffer_upload(Uploads& uploads, const UploadMemory& memory,
    VkBufferUsageFlags usage, std::span<const std::byte> bytes) {
    const auto& properties = memory.properties;
    if (bytes.empty()) return std::nullopt;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes.size();
    info.usage = usage;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (const auto result = vkCreateBuffer(uploads.device, &info, nullptr, &buffer);
        result != VK_SUCCESS) return failure(result, "Create Vulkan upload buffer");
    uploads.buffers.push_back({buffer, VK_NULL_HANDLE,
        !!(usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT), false});
    auto& owned = uploads.buffers.back();
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(uploads.device, owned.handle, &requirements);
    const auto index = memory_type(properties, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!index)
        return Error{ErrorCode::unsupported, "No Vulkan host-visible upload memory"};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *index;
    if (const auto result = vkAllocateMemory(uploads.device, &allocate, nullptr, &owned.memory);
        result != VK_SUCCESS) return failure(result, "Allocate Vulkan upload buffer");
    if (const auto result = vkBindBufferMemory(uploads.device, owned.handle, owned.memory, 0);
        result != VK_SUCCESS) return failure(result, "Bind Vulkan upload buffer");
    if (bytes.size() > allocate.allocationSize)
        return Error{ErrorCode::capacity, "Vulkan upload exceeds allocated buffer"};
    void* mapped = nullptr;
    if (const auto result = vkMapMemory(uploads.device, owned.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        result != VK_SUCCESS) return failure(result, "Map Vulkan upload buffer");
    std::memcpy(mapped, bytes.data(), bytes.size());
    const bool coherent = !!(properties.memoryTypes[*index].propertyFlags &
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    owned.coherent = coherent;
    VkResult flush = VK_SUCCESS;
    if (!coherent || memory.force_explicit_flush) {
        const auto aligned = detail::atom_aligned_range(0, bytes.size(), memory.atom,
            allocate.allocationSize);
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = owned.memory;
        range.offset = aligned.offset;
        range.size = aligned.size;
        flush = vkFlushMappedMemoryRanges(uploads.device, 1, &range);
        owned.flushed = true;
    }
    vkUnmapMemory(uploads.device, owned.memory);
    if (flush != VK_SUCCESS) return failure(flush, "Flush Vulkan upload buffer");
    return std::nullopt;
}

Status texture_upload(Uploads& uploads, const VkPhysicalDeviceMemoryProperties& properties,
    const TextureSnapshot& texture) {
    auto& image = uploads.images.emplace_back();
    image.revision = texture.revision;
    image.width = texture.width;
    image.height = texture.height;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {texture.width, texture.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (const auto result = vkCreateImage(uploads.device, &info, nullptr, &image.handle);
        result != VK_SUCCESS) return failure(result, "Create Vulkan texture image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(uploads.device, image.handle, &requirements);
    const auto index = memory_type(properties, requirements.memoryTypeBits,
        0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!index) return Error{ErrorCode::unsupported, "No compatible Vulkan image memory"};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *index;
    if (const auto result = vkAllocateMemory(uploads.device, &allocate, nullptr, &image.memory);
        result != VK_SUCCESS) return failure(result, "Allocate Vulkan texture image");
    if (const auto result = vkBindImageMemory(uploads.device, image.handle, image.memory, 0);
        result != VK_SUCCESS) return failure(result, "Bind Vulkan texture image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
    if (const auto result = vkCreateImageView(uploads.device, &view, nullptr, &image.view);
        result != VK_SUCCESS) return failure(result, "Create Vulkan texture view");
    return std::nullopt;
}

void record_texture_copy(VkCommandBuffer commands, const Uploads::Image& image) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.handle;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {image.width, image.height, 1};
    vkCmdCopyBufferToImage(commands, image.staging, image.handle,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace
struct Renderer::Impl {
    struct Slot {
        detail::SlotState state;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        bool descriptors_dirty = false;
        std::unique_ptr<Uploads> uploads;
        DrawPacket packet;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence completion = VK_NULL_HANDLE;
    };

    Config config;
    PFN_vkGetFenceStatus fence_status = &vkGetFenceStatus;
    UploadMemory memory;
    VkDescriptorSetLayout texture_layout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    detail::Pipeline pipeline;
    std::vector<Slot> slots;
    bool closed = false;
    bool lost = false;

    ~Impl() {
        if (config.device == VK_NULL_HANDLE) return;
        for (auto& slot : slots) {
            slot.uploads.reset();
            if (slot.descriptors != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(config.device, slot.descriptors, nullptr);
        }
        detail::destroy_pipeline(config.device, pipeline);
        if (sampler) vkDestroySampler(config.device, sampler, nullptr);
        if (texture_layout) vkDestroyDescriptorSetLayout(config.device, texture_layout, nullptr);
    }
    void lose() noexcept {
        lost = true;
        for (auto& slot : slots) slot.state.lose();
    }
};

PreparedFrame::PreparedFrame(Renderer* owner, std::uint32_t index) noexcept
    : owner_(owner), index_(index) {}

PreparedFrame::PreparedFrame(PreparedFrame&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {}

PreparedFrame& PreparedFrame::operator=(PreparedFrame&& other) noexcept {
    if (this != &other) {
        if (owner_) owner_->abandon(std::move(*this));
        owner_ = std::exchange(other.owner_, nullptr);
        index_ = other.index_;
    }
    return *this;
}

PreparedFrame::~PreparedFrame() {
    // The host must have discarded an unsubmitted command buffer before this point.
    if (owner_) owner_->abandon(std::move(*this));
}

Result<std::unique_ptr<Renderer>> Renderer::create(Config config) {
    if (config.physical_device == VK_NULL_HANDLE || config.device == VK_NULL_HANDLE ||
        config.render_pass == VK_NULL_HANDLE || config.max_frames == 0)
        return Error{ErrorCode::invalid_argument, "Vulkan device, render pass and frames are required"};
    if (config.format != VK_FORMAT_R8G8B8A8_UNORM || config.samples != VK_SAMPLE_COUNT_1_BIT)
        return Error{ErrorCode::unsupported, "Vulkan probe requires single-sample RGBA8 UNORM"};

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(config.physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1)
        return Error{ErrorCode::unsupported, "Vulkan 1.1 device required"};
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(config.physical_device, config.format, &format);
    constexpr auto features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & features) != features ||
        !(properties.limits.framebufferColorSampleCounts & config.samples))
        return Error{ErrorCode::unsupported, "Vulkan RGBA8 attachment and sampled format unsupported"};

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(config.physical_device, &family_count, nullptr);
    if (config.graphics_family >= family_count)
        return Error{ErrorCode::invalid_argument, "Vulkan graphics queue family is out of range"};
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(config.physical_device, &family_count, families.data());
    if (!(families[config.graphics_family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
        families[config.graphics_family].queueCount == 0)
        return Error{ErrorCode::unsupported, "Vulkan queue family has no graphics queue"};

    auto impl = std::make_unique<Impl>();
    impl->config = config;
    vkGetPhysicalDeviceMemoryProperties(config.physical_device, &impl->memory.properties);
    impl->memory.atom = std::max<VkDeviceSize>(properties.limits.nonCoherentAtomSize, 1);
    impl->memory.force_explicit_flush = config.force_explicit_flush;
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    if (const auto result = vkCreateDescriptorSetLayout(config.device, &layout, nullptr,
            &impl->texture_layout); result != VK_SUCCESS)
        return *failure(result, "Create Vulkan texture descriptor layout");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (const auto result = vkCreateSampler(config.device, &sampler, nullptr, &impl->sampler);
        result != VK_SUCCESS) return *failure(result, "Create Vulkan texture sampler");
    if (const auto error = detail::create_pipeline(config, impl->texture_layout,
            impl->pipeline)) return *error;
    impl->slots.resize(config.max_frames);
    for (auto& slot : impl->slots) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = 256;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        const VkResult result = vkCreateDescriptorPool(config.device, &info, nullptr, &pool);
        if (result != VK_SUCCESS) return *failure(result, "Create Vulkan descriptor pool");
        slot.descriptors = pool;
    }
    auto renderer = std::unique_ptr<Renderer>(new Renderer);
    renderer->impl_ = std::move(impl);
    return renderer;
}

Result<PreparedFrame> Renderer::prepare(FrameSlot slot, const DrawPacket& packet) {
    if (impl_ && impl_->lost)
        return Error{ErrorCode::device_lost, "Vulkan device is lost"};
    if (!impl_ || impl_->closed)
        return Error{ErrorCode::invalid_frame, "Vulkan renderer is shut down"};
    if (slot.index >= impl_->slots.size() || slot.commands == VK_NULL_HANDLE ||
        slot.completion == VK_NULL_HANDLE)
        return Error{ErrorCode::invalid_argument, "Invalid Vulkan frame slot"};
    auto& entry = impl_->slots[slot.index];
    if (entry.state.state != detail::TicketState::free)
        return Error{ErrorCode::busy, "Vulkan frame slot is in use"};
    if (!packet) return Error{ErrorCode::invalid_packet, "Null Vulkan draw packet"};
    if (const auto error = validate_packet(*packet)) return *error;
    if (packet->textures.size() > 256)
        return Error{ErrorCode::capacity, "Vulkan frame has more than 256 textures"};
    const VkResult fence = impl_->fence_status(impl_->config.device, slot.completion);
    if (fence == VK_ERROR_DEVICE_LOST) {
        impl_->lose();
        return Error{ErrorCode::device_lost, "Vulkan device is lost"};
    }
    if (fence == VK_SUCCESS)
        return Error{ErrorCode::invalid_frame, "Host must reset the frame fence before prepare"};
    if (fence != VK_NOT_READY) return *failure(fence, "Check Vulkan completion fence");
    if (entry.descriptors_dirty) {
        const VkResult reset = vkResetDescriptorPool(impl_->config.device, entry.descriptors, 0);
        if (reset == VK_ERROR_DEVICE_LOST) impl_->lose();
        if (reset != VK_SUCCESS) return *failure(reset, "Reset Vulkan frame descriptors");
        entry.descriptors_dirty = false;
    }
    std::unique_ptr<Uploads> uploads;
    if (!packet->vertices.empty() || !packet->indices.empty() || !packet->textures.empty()) {
        uploads = std::make_unique<Uploads>(impl_->config.device);
        uploads->buffers.reserve(2 + packet->textures.size());
        uploads->images.reserve(packet->textures.size());
        const auto vertices = std::as_bytes(std::span<const Vertex>(packet->vertices));
        const auto indices = std::as_bytes(std::span<const std::uint32_t>(packet->indices));
        if (const auto error = buffer_upload(*uploads, impl_->memory,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices)) {
            if (error->code == ErrorCode::device_lost) impl_->lose();
            return *error;
        }
        if (!vertices.empty()) uploads->vertices = uploads->buffers.back().handle;
        if (const auto error = buffer_upload(*uploads, impl_->memory,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices)) {
            if (error->code == ErrorCode::device_lost) impl_->lose();
            return *error;
        }
        if (!indices.empty()) uploads->indices = uploads->buffers.back().handle;
        for (const auto& texture : packet->textures) {
            if (const auto error = buffer_upload(*uploads, impl_->memory,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, texture->rgba)) {
                if (error->code == ErrorCode::device_lost) impl_->lose();
                return *error;
            }
            const auto staging = uploads->buffers.back().handle;
            if (const auto error = texture_upload(*uploads, impl_->memory.properties, *texture)) {
                if (error->code == ErrorCode::device_lost) impl_->lose();
                return *error;
            }
            auto& image = uploads->images.back();
            image.staging = staging;
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = entry.descriptors;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &impl_->texture_layout;
            entry.descriptors_dirty = true;
            const VkResult result = vkAllocateDescriptorSets(impl_->config.device, &allocate,
                &image.descriptor);
            if (result == VK_ERROR_DEVICE_LOST) impl_->lose();
            if (result != VK_SUCCESS) return *failure(result, "Allocate Vulkan texture descriptor");
            VkDescriptorImageInfo info{impl_->sampler, image.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = image.descriptor;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(impl_->config.device, 1, &write, 0, nullptr);
        }
        for (const auto& image : uploads->images) record_texture_copy(slot.commands, image);
    }
    (void)entry.state.prepare(); // Free was checked above; this cannot fail.
    entry.uploads = std::move(uploads);
    entry.packet = packet;
    entry.commands = slot.commands;
    entry.completion = slot.completion;
    return PreparedFrame(this, slot.index);
}

Status Renderer::record(const PreparedFrame& prepared, Target target) {
    if (impl_ && impl_->lost)
        return Error{ErrorCode::device_lost, "Vulkan device is lost"};
    if (!impl_ || impl_->closed || prepared.owner_ != this ||
        prepared.index_ >= impl_->slots.size())
        return Error{ErrorCode::invalid_frame, "Vulkan frame ticket does not belong to renderer"};
    auto& slot = impl_->slots[prepared.index_];
    const auto& packet = *slot.packet;
    const double width = static_cast<double>(packet.viewport.w) * packet.scale;
    const double height = static_cast<double>(packet.viewport.h) * packet.scale;
    // A different render pass is accepted when render-pass compatible with the
    // pipeline's; the host-declared attachment format and samples must match.
    if (target.render_pass != VK_NULL_HANDLE &&
        (target.format != impl_->config.format || target.samples != impl_->config.samples))
        return Error{ErrorCode::unsupported,
            "Vulkan target attachment format or sample count is incompatible with the UI pipeline"};
    if (target.render_pass == VK_NULL_HANDLE ||
        target.subpass != impl_->config.subpass ||
        target.extent.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        target.extent.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        !(width > 0) || !(height > 0) ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max() ||
        std::round(width) != target.extent.width ||
        std::round(height) != target.extent.height)
        return Error{ErrorCode::invalid_argument, "Incompatible Vulkan render target"};
    if (const auto error = slot.state.record()) return error;
    if (packet.batches.empty() || packet.indices.empty()) return std::nullopt;

    const VkCommandBuffer commands = slot.commands;
    const auto& uploads = *slot.uploads;
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline.graphics);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &uploads.vertices, &offset);
    vkCmdBindIndexBuffer(commands, uploads.indices, 0, VK_INDEX_TYPE_UINT32);
    VkViewport viewport{};
    viewport.width = static_cast<float>(target.extent.width);
    viewport.height = static_cast<float>(target.extent.height); // Positive: NDC -1 is top.
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    vkCmdSetViewport(commands, 0, 1, &viewport);
    detail::PushConstants parameters{{packet.viewport.x, packet.viewport.y,
        packet.viewport.w, packet.viewport.h}, 0};
    for (const auto& batch : packet.batches) {
        if (!batch.index_count || batch.clip.w <= 0 || batch.clip.h <= 0) continue;
        const auto pixels = ::wui::detail::physical_scissor(batch.clip, packet.viewport,
            packet.scale, target.extent.width, target.extent.height);
        if (pixels.empty()) continue;
        const VkRect2D scissor{{pixels.left, pixels.top},
            {static_cast<std::uint32_t>(pixels.right - pixels.left),
             static_cast<std::uint32_t>(pixels.bottom - pixels.top)}};
        vkCmdSetScissor(commands, 0, 1, &scissor);
        const auto& image = uploads.images[batch.texture_index];
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
            impl_->pipeline.layout, 0, 1, &image.descriptor, 0, nullptr);
        parameters.texture_is_straight =
            packet.textures[batch.texture_index]->alpha == AlphaMode::straight ? 1u : 0u;
        vkCmdPushConstants(commands, impl_->pipeline.layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(parameters), &parameters);
        vkCmdDrawIndexed(commands, batch.index_count, 1, batch.first_index, batch.base_vertex, 0);
    }
    return std::nullopt;
}

void Renderer::submitted(PreparedFrame&& prepared) noexcept {
    if (!impl_ || impl_->closed || prepared.owner_ != this ||
        prepared.index_ >= impl_->slots.size()) return;
    auto& slot = impl_->slots[prepared.index_];
    if (slot.state.state != detail::TicketState::recorded) return;
    (void)slot.state.submit(); // The guarded valid transition cannot fail or allocate.
    prepared.owner_ = nullptr;
}

void Renderer::abandon(PreparedFrame&& prepared) noexcept {
    if (!impl_ || prepared.owner_ != this || prepared.index_ >= impl_->slots.size()) return;
    auto& slot = impl_->slots[prepared.index_];
    if (slot.state.state == detail::TicketState::prepared ||
        slot.state.state == detail::TicketState::recorded) {
        (void)slot.state.abandon(true);
        slot.packet.reset();
        slot.uploads.reset();
        slot.commands = VK_NULL_HANDLE;
        slot.completion = VK_NULL_HANDLE;
    }
    prepared.owner_ = nullptr;
}

Status Renderer::collect() {
    if (impl_ && impl_->lost) return Error{ErrorCode::device_lost, "Vulkan device is lost"};
    if (!impl_ || impl_->closed) return std::nullopt;
    for (auto& slot : impl_->slots) {
        if (slot.state.state != detail::TicketState::submitted) continue;
        const VkResult fence = impl_->fence_status(impl_->config.device, slot.completion);
        if (fence == VK_NOT_READY) continue;
        if (fence == VK_ERROR_DEVICE_LOST) {
            impl_->lose();
            return Error{ErrorCode::device_lost, "Vulkan device is lost"};
        }
        if (fence != VK_SUCCESS) return failure(fence, "Poll Vulkan frame fence");
        (void)slot.state.retire(true);
        slot.packet.reset();
        slot.uploads.reset();
        slot.commands = VK_NULL_HANDLE;
        slot.completion = VK_NULL_HANDLE;
    }
    return std::nullopt;
}

Status Renderer::shutdown() {
    if (impl_ && impl_->lost && impl_->closed)
        return Error{ErrorCode::device_lost, "Vulkan device is lost"};
    if (!impl_ || impl_->closed) return std::nullopt;
    if (const auto error = collect()) {
        if (error->code != ErrorCode::device_lost) return error;
        impl_->closed = true; // Terminal device loss: do not poll fences again.
        return error;
    }
    for (const auto& slot : impl_->slots)
        if (slot.state.state != detail::TicketState::free)
            return Error{ErrorCode::busy, "Vulkan frame slots still in use"};
    impl_->closed = true;
    return std::nullopt;
}

Renderer::~Renderer() {
    if (impl_ && !impl_->closed) {
        const auto status = shutdown();
        assert((!status || status->code == ErrorCode::device_lost) &&
            "Host must complete or abandon every Vulkan ticket before destruction");
        (void)status;
    }
}
detail::SlotView detail::Inspection::slot(const Renderer& renderer, std::uint32_t index) {
    SlotView view;
    if (!renderer.impl_ || index >= renderer.impl_->slots.size()) return view;
    const auto& uploads = renderer.impl_->slots[index].uploads;
    if (!uploads) return view;
    view.buffers = uploads->buffers.size();
    for (const auto& buffer : uploads->buffers) {
        view.noncoherent_staging |= buffer.staging && !buffer.coherent;
        view.noncoherent += buffer.coherent ? 0u : 1u;
        view.flushed += buffer.flushed ? 1u : 0u;
    }
    view.textures.reserve(uploads->images.size());
    for (const auto& image : uploads->images)
        view.textures.push_back({image.handle, image.revision});
    return view;
}

PFN_vkGetFenceStatus detail::Inspection::replace_fence_status(
    Renderer& renderer, PFN_vkGetFenceStatus replacement) noexcept {
    auto& selected = renderer.impl_->fence_status;
    return std::exchange(selected, replacement);
}


} // namespace wui::vulkan
