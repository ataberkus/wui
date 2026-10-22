#include <doctest/doctest.h>

#include <wui/backends/vulkan.hpp>
#include "../support/native_image.hpp"
#include "../support/packet_fixture.hpp"
#include "../../backends/renderer/vulkan_upload.hpp"
#ifdef WUI_PROBE_TEXT
#include <wui/context.hpp>
#include <wui/text.hpp>
#include "../../examples/widget_gallery/gallery.hpp"
#include <filesystem>
#include <fstream>
#endif

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <iostream>
#include <variant>
#include <utility>
#include <vector>

namespace {

void checked(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

VKAPI_ATTR VkResult VKAPI_CALL simulated_fence_pending(VkDevice, VkFence) {
    return VK_NOT_READY;
}

template<class T>
T success(wui::Result<T>& result) {
    if (const auto* error = std::get_if<wui::Error>(&result))
        throw std::runtime_error(error->message);
    return std::move(std::get<T>(result));
}

std::uint32_t memory_type(VkPhysicalDevice physical, std::uint32_t bits,
    VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("No compatible Vulkan image/readback memory type");
}

struct NativeHarness {
    const std::uint32_t size;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkImage color = VK_NULL_HANDLE;
    VkDeviceMemory color_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    bool readback_coherent = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 3> commands{};
    std::array<VkFence, 3> fences{};
    std::vector<std::string> validation_messages;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* user) {
        if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
            static_cast<NativeHarness*>(user)->validation_messages.emplace_back(data->pMessage);
        return VK_FALSE;
    }

    explicit NativeHarness(std::uint32_t extent = 128) : size(extent) {
        std::uint32_t instance_version = VK_API_VERSION_1_0;
        checked(vkEnumerateInstanceVersion(&instance_version), "vkEnumerateInstanceVersion");
        if (instance_version < VK_API_VERSION_1_1)
            throw std::runtime_error("Vulkan 1.1 loader required");

        std::uint32_t count = 0;
        checked(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate instance layers");
        std::vector<VkLayerProperties> layers(count);
        checked(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate instance layers");
        bool layer_found = false;
        for (const auto& layer : layers)
            if (std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation") layer_found = true;
        if (!layer_found) throw std::runtime_error("VK_LAYER_KHRONOS_validation is required");

        checked(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
            "enumerate instance extensions");
        std::vector<VkExtensionProperties> extensions(count);
        checked(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
            "enumerate instance extensions");
        bool debug_found = false;
        for (const auto& extension : extensions)
            if (std::string(extension.extensionName) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
                debug_found = true;
        if (!debug_found) throw std::runtime_error("VK_EXT_debug_utils is required");
        count = 0;
        const char* enabled_layer = "VK_LAYER_KHRONOS_validation";
        checked(vkEnumerateInstanceExtensionProperties(enabled_layer, &count, nullptr),
            "enumerate validation layer extensions");
        std::vector<VkExtensionProperties> layer_extensions(count);
        checked(vkEnumerateInstanceExtensionProperties(enabled_layer, &count,
            layer_extensions.data()), "enumerate validation layer extensions");
        bool sync_found = false;
        for (const auto& extension : layer_extensions)
            if (std::string(extension.extensionName) == VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
                sync_found = true;
        if (!sync_found)
            throw std::runtime_error("VK_EXT_validation_features is required for synchronization validation");

        const std::array<const char*, 2> enabled_extensions{
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount = 1;
        validation.pEnabledValidationFeatures = &sync;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pNext = &validation;
        info.pApplicationInfo = &app;
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &enabled_layer;
        info.enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size());
        info.ppEnabledExtensionNames = enabled_extensions.data();
        checked(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
        try { initialize(); } catch (...) { cleanup(); throw; }
    }

    void initialize() {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!create_messenger) throw std::runtime_error("debug_utils messenger unavailable");
        VkDebugUtilsMessengerCreateInfoEXT debug_info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_info.pfnUserCallback = &debug;
        debug_info.pUserData = this;
        checked(create_messenger(instance, &debug_info, nullptr, &messenger),
            "vkCreateDebugUtilsMessengerEXT");

        std::uint32_t count = 0;
        checked(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        if (!count) throw std::runtime_error("No Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        checked(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1) continue;
            VkFormatProperties color_format{};
            vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R8G8B8A8_UNORM, &color_format);
            const auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if ((color_format.optimalTilingFeatures & required) != required ||
                !(properties.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_1_BIT)) continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (std::uint32_t i = 0; i < families; ++i) {
                if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queues[i].queueCount) {
                    physical = candidate;
                    family = i;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical)
            throw std::runtime_error("No Vulkan 1.1 graphics device with RGBA8 color, sampled and transfer-source support");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        checked(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass_info.attachmentCount = 1;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;
        checked(vkCreateRenderPass(device, &pass_info, nullptr, &pass), "vkCreateRenderPass");

        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {size, size, 1};
        image_info.mipLevels = image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        checked(vkCreateImage(device, &image_info, nullptr, &color), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, color, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device, &allocation, nullptr, &color_memory), "allocate color memory");
        checked(vkBindImageMemory(device, color, color_memory, 0), "bind color memory");

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = color;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = view_info.subresourceRange.layerCount = 1;
        checked(vkCreateImageView(device, &view_info, nullptr, &view), "vkCreateImageView");
        VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb_info.renderPass = pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &view;
        fb_info.width = fb_info.height = size;
        fb_info.layers = 1;
        checked(vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffer), "vkCreateFramebuffer");

        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = size * size * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateBuffer(device, &buffer_info, nullptr, &readback), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(device, readback, &requirements);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        readback_coherent = !!(memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checked(vkAllocateMemory(device, &allocation, nullptr, &readback_memory),
            "allocate readback memory");
        checked(vkBindBufferMemory(device, readback, readback_memory, 0), "bind readback memory");

        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        checked(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = static_cast<std::uint32_t>(commands.size());
        checked(vkAllocateCommandBuffers(device, &command_info, commands.data()),
            "allocate primary command buffers");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (auto& fence : fences)
            checked(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
    }

    ~NativeHarness() { cleanup(); }
    void cleanup() {
        if (device) {
            for (auto fence : fences) if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (readback) vkDestroyBuffer(device, readback, nullptr);
            if (readback_memory) vkFreeMemory(device, readback_memory, nullptr);
            if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (view) vkDestroyImageView(device, view, nullptr);
            if (color) vkDestroyImage(device, color, nullptr);
            if (color_memory) vkFreeMemory(device, color_memory, nullptr);
            if (pass) vkDestroyRenderPass(device, pass, nullptr);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
            messenger = VK_NULL_HANDLE;
        }
        if (instance) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    wui::vulkan::Config config() const {
        return {physical, device, family, pass};
    }
    void begin(std::uint32_t slot) {
        checked(vkResetFences(device, 1, &fences[slot]), "reset host fence");
        checked(vkResetCommandBuffer(commands[slot], 0), "reset host command buffer");
        VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        checked(vkBeginCommandBuffer(commands[slot], &begin_info), "begin host command buffer");
    }
    void begin_pass(std::uint32_t slot) {
        VkClearValue clear{};
        clear.color = {{0, 0, 1, 1}};
        VkRenderPassBeginInfo begin_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin_info.renderPass = pass;
        begin_info.framebuffer = framebuffer;
        begin_info.renderArea.extent = {size, size};
        begin_info.clearValueCount = 1;
        begin_info.pClearValues = &clear;
        vkCmdBeginRenderPass(commands[slot], &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    }
    void copy_readback(std::uint32_t slot) {
        vkCmdEndRenderPass(commands[slot]);
        VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image_barrier.srcQueueFamilyIndex = image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = color;
        image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.levelCount = image_barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commands[slot], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image_barrier);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {size, size, 1};
        vkCmdCopyImageToBuffer(commands[slot], color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback, 1, &region);
        VkBufferMemoryBarrier buffer_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buffer_barrier.srcQueueFamilyIndex = buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.buffer = readback;
        buffer_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commands[slot], VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
    }
    void submit(std::uint32_t slot) {
        checked(vkEndCommandBuffer(commands[slot]), "end host command buffer");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &commands[slot];
        checked(vkQueueSubmit(queue, 1, &info, fences[slot]), "host vkQueueSubmit");
    }
    void wait(std::uint32_t slot) {
        checked(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, 5'000'000'000ULL),
            "wait for host fence");
    }
    wui::test::Image pixels() {
        void* mapped = nullptr;
        checked(vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped),
            "map readback");
        if (!readback_coherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = readback_memory;
            range.size = VK_WHOLE_SIZE;
            const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) {
                vkUnmapMemory(device, readback_memory);
                checked(result, "invalidate readback");
            }
        }
        wui::test::Image image;
        image.width = image.height = size;
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        image.pixels.reserve(size * size);
        for (std::uint32_t i = 0; i < size * size; ++i)
            image.pixels.push_back({bytes[4*i], bytes[4*i+1], bytes[4*i+2], bytes[4*i+3]});
        vkUnmapMemory(device, readback_memory);
        return image;
    }
    void check_validation() const {
        for (const auto& message : validation_messages)
            CHECK_MESSAGE(false, "Vulkan validation warning/error: ", message);
        CHECK(validation_messages.empty());
    }
};

void check_channel(std::uint8_t actual, std::uint8_t expected, int tolerance = 2) {
    CHECK(std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= tolerance);
}

void check_pixel(wui::test::Pixel actual, std::uint8_t r, std::uint8_t g,
    std::uint8_t b, std::uint8_t a, int tolerance = 2) {
    check_channel(actual.r, r, tolerance);
    check_channel(actual.g, g, tolerance);
    check_channel(actual.b, b, tolerance);
    check_channel(actual.a, a, tolerance);
}

wui::test::Image render_reference(NativeHarness& native, wui::vulkan::Renderer& renderer,
    const wui::DrawPacket& packet) {
    native.begin(0);
    auto prepared = renderer.prepare({0, native.commands[0], native.fences[0]}, packet);
    auto ticket = success(prepared);
    native.begin_pass(0);
    if (const auto error = renderer.record(ticket, {native.pass, 0, {native.size, native.size}})) {
        checked(vkResetCommandBuffer(native.commands[0], 0), "discard failed host recording");
        renderer.abandon(std::move(ticket));
        throw std::runtime_error("Record Vulkan reference packet: " + error->message);
    }
    native.copy_readback(0);
    try {
        native.submit(0);
    } catch (...) {
        checked(vkResetCommandBuffer(native.commands[0], 0), "discard failed submission");
        renderer.abandon(std::move(ticket));
        throw;
    }
    renderer.submitted(std::move(ticket));
    native.wait(0);
    if (const auto error = renderer.collect())
        throw std::runtime_error("Retire Vulkan reference packet: " + error->message);
    return native.pixels();
}

wui::DrawPacket empty_packet() {
    auto packet = std::make_shared<wui::PacketData>();
    packet->viewport = {0, 0, 128, 128};
    return packet;
}

void expect_error(const wui::Result<std::unique_ptr<wui::vulkan::Renderer>>& result,
    wui::ErrorCode code) {
    REQUIRE(std::holds_alternative<wui::Error>(result));
    CHECK(std::get<wui::Error>(result).code == code);
}

} // namespace

TEST_CASE("Vulkan 1.1 rejects invalid backend configuration") {
    NativeHarness native;
    auto config = native.config();
    config.device = VK_NULL_HANDLE;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::invalid_argument);
    config = native.config();
    config.physical_device = VK_NULL_HANDLE;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::invalid_argument);
    config = native.config();
    config.render_pass = VK_NULL_HANDLE;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::invalid_argument);
    config = native.config();
    config.max_frames = 0;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::invalid_argument);
    config = native.config();
    config.graphics_family = UINT32_MAX;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::invalid_argument);
    config = native.config();
    config.format = VK_FORMAT_B8G8R8A8_UNORM;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::unsupported);
    config = native.config();
    config.samples = VK_SAMPLE_COUNT_4_BIT;
    expect_error(wui::vulkan::Renderer::create(config), wui::ErrorCode::unsupported);
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan host owns submission and the renderer retires completed empty frames") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto packet = empty_packet();
    auto still_signaled = renderer->prepare(
        {0, native.commands[0], native.fences[0]}, packet);
    REQUIRE(std::holds_alternative<wui::Error>(still_signaled));
    CHECK(std::get<wui::Error>(still_signaled).code == wui::ErrorCode::invalid_frame);
    native.begin(0);
    auto prepared = wui::vulkan::FrameSlot{0, native.commands[0], native.fences[0]};
    auto null_packet = renderer->prepare(prepared, {});
    REQUIRE(std::holds_alternative<wui::Error>(null_packet));
    CHECK(std::get<wui::Error>(null_packet).code == wui::ErrorCode::invalid_packet);
    auto ticket_result = renderer->prepare(prepared, packet);
    auto ticket = success(ticket_result);
    auto reused = renderer->prepare(prepared, packet);
    REQUIRE(std::holds_alternative<wui::Error>(reused));
    CHECK(std::get<wui::Error>(reused).code == wui::ErrorCode::busy);
    const auto busy = renderer->shutdown();
    REQUIRE(busy.has_value());
    CHECK(busy->code == wui::ErrorCode::busy);
    native.begin_pass(0);
    const wui::vulkan::Target target{native.pass, 0, {128, 128}};
    CHECK_FALSE(renderer->record(ticket, target).has_value());
    native.copy_readback(0);
    native.submit(0);
    renderer->submitted(std::move(ticket));
    native.wait(0);
    CHECK_FALSE(renderer->collect().has_value());
    const auto image = native.pixels();
    for (const auto point : {std::array<std::uint32_t, 2>{0, 0}, {64, 64}, {127, 127}}) {
        const auto pixel = image.at(point[0], point[1]);
        CHECK(pixel.r == 0);
        CHECK(pixel.g == 0);
        CHECK(pixel.b == 255);
        CHECK(pixel.a == 255);
    }
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan slot cancellation and invalid targets preserve ownership") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto packet = empty_packet();
    native.begin(1);
    auto slot = wui::vulkan::FrameSlot{1, native.commands[1], native.fences[1]};
    auto ticket_result = renderer->prepare(slot, packet);
    auto ticket = success(ticket_result);
    const auto invalid = renderer->record(ticket, {VK_NULL_HANDLE, 0, {128, 128}});
    REQUIRE(invalid.has_value());
    CHECK(invalid->code == wui::ErrorCode::invalid_argument);
    native.begin_pass(1);
    CHECK_FALSE(renderer->record(ticket, {native.pass, 0, {128, 128}}).has_value());
    vkCmdEndRenderPass(native.commands[1]);
    checked(vkResetCommandBuffer(native.commands[1], 0), "discard host command buffer");
    renderer->abandon(std::move(ticket));
    native.begin(1);
    auto next = renderer->prepare(slot, packet);
    REQUIRE(std::holds_alternative<wui::vulkan::PreparedFrame>(next));
    checked(vkResetCommandBuffer(native.commands[1], 0), "discard next command buffer");
    renderer->abandon(std::move(std::get<wui::vulkan::PreparedFrame>(next)));
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan reserves three independent host frame slots") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto packet = empty_packet();
    std::vector<wui::vulkan::PreparedFrame> tickets;
    for (std::uint32_t i = 0; i < 3; ++i) {
        native.begin(i);
        auto result = renderer->prepare({i, native.commands[i], native.fences[i]}, packet);
        tickets.push_back(success(result));
    }
    auto outside = renderer->prepare({3, native.commands[0], native.fences[0]}, packet);
    REQUIRE(std::holds_alternative<wui::Error>(outside));
    CHECK(std::get<wui::Error>(outside).code == wui::ErrorCode::invalid_argument);
    for (std::uint32_t i = 0; i < 3; ++i) {
        checked(vkResetCommandBuffer(native.commands[i], 0), "discard host frame");
        renderer->abandon(std::move(tickets[i]));
    }
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan uploads preserve old snapshots and distinct native texture revisions") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto one = wui::test::reference_packet(1);
    const auto two = wui::test::reference_packet(2);
    std::size_t checker = 0;
    for (; checker < one->textures.size(); ++checker)
        if (one->textures[checker]->width == 4) break;
    REQUIRE(checker < one->textures.size());
    const auto old_pixels = one->textures[checker]->rgba;

    auto malformed = std::make_shared<wui::PacketData>(*one);
    malformed->batches[0].texture_index =
        static_cast<std::uint32_t>(malformed->textures.size());
    auto rejected = renderer->prepare({2, native.commands[2], native.fences[2]}, malformed);
    REQUIRE(std::holds_alternative<wui::Error>(rejected));
    CHECK(std::get<wui::Error>(rejected).code == wui::ErrorCode::invalid_packet);
    // Slot 2 has a signaled fence and a non-recording command buffer. Validation
    // must fail before trying either Vulkan submission or upload recording.

    native.begin(0);
    auto first_result = renderer->prepare({0, native.commands[0], native.fences[0]}, one);
    auto first = success(first_result);
    const auto first_view = wui::vulkan::detail::Inspection::slot(*renderer, 0);
    VkPhysicalDeviceProperties selected{};
    vkGetPhysicalDeviceProperties(native.physical, &selected);
    std::cout << "Vulkan native device=" << selected.deviceName
        << " api=" << VK_VERSION_MAJOR(selected.apiVersion) << '.'
        << VK_VERSION_MINOR(selected.apiVersion) << '.'
        << VK_VERSION_PATCH(selected.apiVersion)
        << " driver=" << selected.driverVersion
        << " graphics_family=" << native.family
        << " validation=VK_LAYER_KHRONOS_validation sync_validation=on"
        << " staging_noncoherent=" << (first_view.noncoherent_staging ? "yes" : "no")
        << " readback_noncoherent=" << (!native.readback_coherent ? "yes" : "no")
        << '\n';
    REQUIRE(first_view.textures.size() == one->textures.size());
    CHECK(first_view.buffers >= 2 + one->textures.size());
    CHECK(first_view.textures[checker].revision == 1);
    CHECK(first_view.textures[checker].image != VK_NULL_HANDLE);
    auto busy = renderer->prepare({0, native.commands[0], native.fences[0]}, two);
    REQUIRE(std::holds_alternative<wui::Error>(busy));
    CHECK(std::get<wui::Error>(busy).code == wui::ErrorCode::busy);

    native.begin(1);
    auto second_result = renderer->prepare({1, native.commands[1], native.fences[1]}, two);
    auto second = success(second_result);
    const auto second_view = wui::vulkan::detail::Inspection::slot(*renderer, 1);
    REQUIRE(second_view.textures.size() == two->textures.size());
    CHECK(second_view.textures[checker].revision == 2);
    CHECK(second_view.textures[checker].image != VK_NULL_HANDLE);
    CHECK(first_view.textures[checker].image != second_view.textures[checker].image);
    CHECK(one->textures[checker]->rgba == old_pixels);
    CHECK(one->textures[checker]->rgba != two->textures[checker]->rgba);

    // These command buffers contain only uploads; BV.3 adds the draw pass.
    // Host waits for both uploads before discarding and releasing their tickets.
    native.submit(0);
    native.submit(1);
    native.wait(0);
    native.wait(1);
    checked(vkResetCommandBuffer(native.commands[0], 0), "discard first upload command buffer");
    checked(vkResetCommandBuffer(native.commands[1], 0), "discard second upload command buffer");
    renderer->abandon(std::move(first));
    renderer->abandon(std::move(second));
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan reference packet draws ordered colors, alpha, clip and checker revisions") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto first = wui::test::reference_packet(1);
    REQUIRE(first->batches.size() >= 2);
    REQUIRE(first->batches[1].base_vertex != 0);
    const auto image = render_reference(native, *renderer, first);
    check_pixel(image.at(12, 12), 255, 0, 0, 255);
    check_pixel(image.at(30, 30), 128, 128, 0, 255, 8);
    check_pixel(image.at(70, 60), 26, 26, 26, 255, 8);
    check_pixel(image.at(100, 100), 26, 26, 26, 255, 8);
    check_pixel(image.at(84, 76), 0, 0, 255, 255, 8);
    check_pixel(image.at(12, 116), 26, 26, 26, 255, 8);
    check_pixel(image.at(75, 11), 255, 255, 255, 255, 8);
    check_pixel(image.at(76, 12), 225, 225, 225, 255, 3);

    const auto second = wui::test::reference_packet(2);
    const auto revised = render_reference(native, *renderer, second);
    check_pixel(revised.at(75, 11), 255, 0, 0, 255, 8);
    check_pixel(revised.at(76, 12), 225, 1, 1, 255, 3);
    check_pixel(revised.at(84, 12), 30, 30, 30, 255, 3);
    CHECK_FALSE(wui::test::write_ppm(image, "vulkan-rev1-scale1.ppm").has_value());
    CHECK_FALSE(wui::test::write_ppm(revised, "vulkan-rev2-scale1.ppm").has_value());
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan renders scale 2 and rejects incompatible targets without consuming tickets") {
    NativeHarness native(256);
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto packet = wui::test::reference_packet(1, 2.0f);
    native.begin(0);
    auto prepared = renderer->prepare({0, native.commands[0], native.fences[0]}, packet);
    auto ticket = success(prepared);
    for (const auto target : {
            wui::vulkan::Target{VK_NULL_HANDLE, 0, {256, 256}},
            wui::vulkan::Target{native.pass, 1, {256, 256}},
            wui::vulkan::Target{native.pass, 0, {128, 128}}}) {
        const auto error = renderer->record(ticket, target);
        REQUIRE(error.has_value());
        CHECK(error->code == wui::ErrorCode::invalid_argument);
    }
    native.begin_pass(0);
    REQUIRE_FALSE(renderer->record(ticket, {native.pass, 0, {256, 256}}).has_value());
    const auto twice = renderer->record(ticket, {native.pass, 0, {256, 256}});
    REQUIRE(twice.has_value());
    CHECK(twice->code == wui::ErrorCode::invalid_frame);
    native.copy_readback(0);
    native.submit(0);
    renderer->submitted(std::move(ticket));
    native.wait(0);
    CHECK_FALSE(renderer->collect().has_value());
    const auto image = native.pixels();
    check_pixel(image.at(24, 24), 255, 0, 0, 255);
    check_pixel(image.at(60, 60), 128, 128, 0, 255, 8);
    check_pixel(image.at(140, 120), 26, 26, 26, 255, 8);
    check_pixel(image.at(24, 232), 26, 26, 26, 255, 8);
    check_pixel(image.at(152, 24), 239, 239, 239, 255, 3);
    check_pixel(image.at(168, 24), 16, 16, 16, 255, 3);
    CHECK_FALSE(wui::test::write_ppm(image, "vulkan-rev1-scale2.ppm").has_value());
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan clips fractional rectangles and ignores empty batches") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    auto packet = std::make_shared<wui::PacketData>(*wui::test::reference_packet(1));
    auto blue = std::find_if(packet->batches.begin(), packet->batches.end(),
        [](const wui::Batch& batch) {
            return batch.clip.x == 80 && batch.clip.y == 72 &&
                batch.clip.w == 16 && batch.clip.h == 16;
        });
    REQUIRE(blue != packet->batches.end());
    blue->clip = {-10.25f, -10.25f, 106.0f, 98.0f};
    packet->batches.push_back({0, 0, 0, 0, {0, 0, 0, 0}});
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    const auto image = render_reference(native, *renderer, packet);
    check_pixel(image.at(95, 87), 0, 0, 255, 255, 8);
    check_pixel(image.at(96, 87), 26, 26, 26, 255, 8);
    check_pixel(image.at(95, 88), 26, 26, 26, 255, 8);
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan retains three submitted revisions until host fence acknowledgment") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto old = wui::test::reference_packet(1);
    const auto newer = wui::test::reference_packet(2);
    auto latest = std::make_shared<wui::PacketData>(*newer);
    std::size_t checker = 0;
    for (; checker < old->textures.size(); ++checker)
        if (old->textures[checker]->width == 4) break;
    REQUIRE(checker < old->textures.size());
    const auto old_bytes = old->textures[checker]->rgba;
    auto changed = std::make_shared<wui::TextureSnapshot>(*newer->textures[checker]);
    changed->revision = 3;
    changed->rgba[0] = std::byte{0};
    changed->rgba[1] = std::byte{255};
    latest->textures[checker] = changed;
    REQUIRE_FALSE(wui::validate_packet(*latest).has_value());
    const std::array<wui::DrawPacket, 3> packets{old, newer, latest};

    std::array<VkImage, 3> image_versions{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        native.begin(i);
        if (i) {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(native.commands[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
        auto prepared = renderer->prepare({i, native.commands[i], native.fences[i]}, packets[i]);
        auto ticket = success(prepared);
        const auto view = wui::vulkan::detail::Inspection::slot(*renderer, i);
        REQUIRE(view.textures.size() == packets[i]->textures.size());
        CHECK(view.textures[checker].revision == i + 1);
        image_versions[i] = view.textures[checker].image;
        native.begin_pass(i);
        REQUIRE_FALSE(renderer->record(ticket, {native.pass, 0, {128, 128}}).has_value());
        native.copy_readback(i);
        native.submit(i);
        renderer->submitted(std::move(ticket));
    }
    CHECK(image_versions[0] != image_versions[1]);
    CHECK(image_versions[1] != image_versions[2]);
    CHECK(image_versions[0] != image_versions[2]);
    CHECK(old->textures[checker]->rgba == old_bytes);
    auto fourth = renderer->prepare({0, native.commands[0], native.fences[0]}, old);
    REQUIRE(std::holds_alternative<wui::Error>(fourth));
    CHECK(std::get<wui::Error>(fourth).code == wui::ErrorCode::busy);
    const auto genuine_fence_status = wui::vulkan::detail::Inspection::replace_fence_status(
        *renderer, &simulated_fence_pending);
    REQUIRE(genuine_fence_status != nullptr);
    CHECK_FALSE(renderer->collect().has_value()); // Simulated NOT_READY, not a timing guess.
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto held = wui::vulkan::detail::Inspection::slot(*renderer, i);
        REQUIRE(held.textures.size() > checker);
        CHECK(held.textures[checker].image == image_versions[i]);
    }
    const auto pending_shutdown = renderer->shutdown();
    REQUIRE(pending_shutdown.has_value());
    CHECK(pending_shutdown->code == wui::ErrorCode::busy);
    wui::vulkan::detail::Inspection::replace_fence_status(*renderer, genuine_fence_status);

    for (std::uint32_t i = 0; i < 3; ++i) native.wait(i);
    const auto latest_image = native.pixels();
    check_pixel(latest_image.at(75, 11), 0, 255, 0, 255, 8);
    CHECK_FALSE(renderer->collect().has_value());
    for (std::uint32_t i = 0; i < 3; ++i)
        CHECK(wui::vulkan::detail::Inspection::slot(*renderer, i).textures.empty());
    CHECK(old->textures[checker]->rgba == old_bytes);
    const auto rerendered = render_reference(native, *renderer, old);
    check_pixel(rerendered.at(75, 11), 255, 255, 255, 255, 8);
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan abandons uploaded resources only after host command discard") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto packet = wui::test::reference_packet(1);
    for (const bool recorded : {false, true}) {
        native.begin(0);
        auto prepared = renderer->prepare({0, native.commands[0], native.fences[0]}, packet);
        auto ticket = success(prepared);
        CHECK_FALSE(wui::vulkan::detail::Inspection::slot(*renderer, 0).textures.empty());
        if (recorded) {
            native.begin_pass(0);
            REQUIRE_FALSE(renderer->record(ticket, {native.pass, 0, {128, 128}}).has_value());
        }
        checked(vkResetCommandBuffer(native.commands[0], 0), "discard abandoned recording");
        renderer->abandon(std::move(ticket));
        CHECK(wui::vulkan::detail::Inspection::slot(*renderer, 0).textures.empty());
    }
    const auto recovered = render_reference(native, *renderer, packet);
    check_pixel(recovered.at(12, 12), 255, 0, 0, 255);
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

namespace {
VKAPI_ATTR VkResult VKAPI_CALL simulated_fence_loss(VkDevice, VkFence) {
    return VK_ERROR_DEVICE_LOST;
}
}

TEST_CASE("simulated Vulkan fence loss is terminal without claiming physical device loss") {
    NativeHarness native;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto packet = wui::test::reference_packet(1);
    native.begin(0);
    auto prepared = renderer->prepare({0, native.commands[0], native.fences[0]}, packet);
    auto ticket = success(prepared);
    native.begin_pass(0);
    REQUIRE_FALSE(renderer->record(ticket, {native.pass, 0, {128, 128}}).has_value());
    native.copy_readback(0);
    native.submit(0);
    renderer->submitted(std::move(ticket));
    native.wait(0); // Real device is healthy; only the next fence query is injected.
    const auto previous = wui::vulkan::detail::Inspection::replace_fence_status(
        *renderer, &simulated_fence_loss);
    REQUIRE(previous != nullptr);
    const auto failed = renderer->collect();
    REQUIRE(failed.has_value());
    CHECK(failed->code == wui::ErrorCode::device_lost);
    auto retry = renderer->prepare({0, native.commands[0], native.fences[0]}, packet);
    REQUIRE(std::holds_alternative<wui::Error>(retry));
    CHECK(std::get<wui::Error>(retry).code == wui::ErrorCode::device_lost);
    const auto terminal = renderer->shutdown();
    REQUIRE(terminal.has_value());
    CHECK(terminal->code == wui::ErrorCode::device_lost);
    renderer.reset();
    auto reconstructed = wui::vulkan::Renderer::create(native.config());
    auto replacement = success(reconstructed);
    const auto image = render_reference(native, *replacement, packet);
    check_pixel(image.at(12, 12), 255, 0, 0, 255);
    CHECK_FALSE(replacement->shutdown().has_value());
    replacement.reset();
    native.cleanup();
    native.check_validation();
}

namespace {
// Records one frame of `packet` with an explicit target and returns the readback.
wui::test::Image render_with(NativeHarness& native, wui::vulkan::Renderer& renderer,
    const wui::DrawPacket& packet, VkRenderPass pass, wui::vulkan::Target target,
    wui::vulkan::detail::SlotView* inspected = nullptr) {
    native.begin(0);
    auto prepared = renderer.prepare({0, native.commands[0], native.fences[0]}, packet);
    auto ticket = success(prepared);
    if (inspected) *inspected = wui::vulkan::detail::Inspection::slot(renderer, 0);
    VkClearValue clear{};
    clear.color = {{0, 0, 1, 1}};
    VkRenderPassBeginInfo begin_info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin_info.renderPass = pass;
    begin_info.framebuffer = native.framebuffer;
    begin_info.renderArea.extent = {native.size, native.size};
    begin_info.clearValueCount = 1;
    begin_info.pClearValues = &clear;
    vkCmdBeginRenderPass(native.commands[0], &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    if (const auto error = renderer.record(ticket, target)) {
        vkCmdEndRenderPass(native.commands[0]);
        checked(vkResetCommandBuffer(native.commands[0], 0), "discard failed host recording");
        renderer.abandon(std::move(ticket));
        throw std::runtime_error("Record Vulkan packet: " + error->message);
    }
    native.copy_readback(0);
    native.submit(0);
    renderer.submitted(std::move(ticket));
    native.wait(0);
    if (const auto error = renderer.collect())
        throw std::runtime_error("Retire Vulkan packet: " + error->message);
    return native.pixels();
}
} // namespace

TEST_CASE("Vulkan force_explicit_flush flushes atom-aligned ranges on every upload buffer") {
    NativeHarness native;
    const auto packet = wui::test::reference_packet(1);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(native.physical, &properties);
    // Index data of the reference packet is not a multiple of the atom, so an
    // unaligned flush on coherent memory is reported by the validation layer.
    INFO("nonCoherentAtomSize=" << properties.limits.nonCoherentAtomSize
        << " index bytes=" << packet->indices.size() * 4);
    for (const bool forced : {false, true}) {
        auto config = native.config();
        config.force_explicit_flush = forced;
        auto created = wui::vulkan::Renderer::create(config);
        auto renderer = success(created);
        wui::vulkan::detail::SlotView view;
        const auto image = render_with(native, *renderer, packet, native.pass,
            {native.pass, 0, {128, 128}}, &view);
        REQUIRE(view.buffers >= 2 + packet->textures.size());
        if (forced) CHECK(view.flushed == view.buffers);
        else CHECK(view.flushed == view.noncoherent);
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        check_pixel(image.at(84, 76), 0, 0, 255, 255, 8);
        check_pixel(image.at(76, 12), 225, 225, 225, 255, 3);
        CHECK_FALSE(renderer->shutdown().has_value());
    }
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan pipeline accepts a compatible render pass and rejects incompatible attachments") {
    NativeHarness native;
    // Compatible with the creation pass: same attachment format, samples and
    // subpass structure; only the attachment-reference layout differs.
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VkRenderPass second = VK_NULL_HANDLE;
    checked(vkCreateRenderPass(native.device, &pass_info, nullptr, &second), "create compatible pass");
    {
        auto created = wui::vulkan::Renderer::create(native.config());
        auto renderer = success(created);
        const auto packet = wui::test::reference_packet(1);
        native.begin(0);
        auto prepared = renderer->prepare({0, native.commands[0], native.fences[0]}, packet);
        auto ticket = success(prepared);
        wui::vulkan::Target wrong_format{second, 0, {128, 128}};
        wrong_format.format = VK_FORMAT_B8G8R8A8_UNORM;
        wui::vulkan::Target wrong_samples{second, 0, {128, 128}};
        wrong_samples.samples = VK_SAMPLE_COUNT_4_BIT;
        for (const auto& target : {wrong_format, wrong_samples}) {
            const auto error = renderer->record(ticket, target);
            REQUIRE(error.has_value());
            CHECK(error->code == wui::ErrorCode::unsupported);
        }
        // Rejections consume nothing: the ticket is still recordable.
        checked(vkResetCommandBuffer(native.commands[0], 0), "discard probe recording");
        renderer->abandon(std::move(ticket));
        const auto image = render_with(native, *renderer, packet, second, {second, 0, {128, 128}});
        check_pixel(image.at(12, 12), 255, 0, 0, 255);
        check_pixel(image.at(84, 76), 0, 0, 255, 255, 8);
        check_pixel(image.at(76, 12), 225, 225, 225, 255, 3);
        CHECK_FALSE(renderer->shutdown().has_value());
    }
    vkDestroyRenderPass(native.device, second, nullptr);
    native.cleanup();
    native.check_validation();
}

TEST_CASE("Vulkan fractional-scale scissor snaps float noise to the pixel edge") {
    NativeHarness native(160);
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    auto packet = std::make_shared<wui::PacketData>(*wui::test::reference_packet(1, 1.25f));
    auto blue = std::find_if(packet->batches.begin(), packet->batches.end(),
        [](const wui::Batch& batch) {
            return batch.clip.x == 80 && batch.clip.y == 72 &&
                batch.clip.w == 16 && batch.clip.h == 16;
        });
    REQUIRE(blue != packet->batches.end());
    // 57.6f * 1.25 is 71.99999809 in double: the clip's top edge is physical row 72.
    blue->clip = {80, 57.6f, 16, 30.4f};
    REQUIRE_FALSE(wui::validate_packet(*packet).has_value());
    const auto image = render_reference(native, *renderer, packet);
    check_pixel(image.at(110, 71), 26, 26, 26, 255, 8);
    check_pixel(image.at(110, 72), 0, 0, 255, 255, 8);
    check_pixel(image.at(110, 109), 0, 0, 255, 255, 8);
    check_pixel(image.at(110, 110), 26, 26, 26, 255, 8);
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    native.cleanup();
    native.check_validation();
}

#ifdef WUI_PROBE_TEXT
namespace {
std::vector<std::byte> font_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open required gallery font: " + path.string());
    const auto length = file.tellg();
    if (length < 0) throw std::runtime_error("Cannot size gallery font: " + path.string());
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("Cannot read gallery font: " + path.string());
    return data;
}

wui::DrawPacket gallery_packet(wui::Context& context, GalleryState& state, bool new_glyph) {
    wui::FrameInput input{{0, 0, 256, 256}, 1.0f, 1.0 / 60.0, {}};
    auto begun = context.begin_frame(input);
    auto frame = success(begun);
    // Emit the new glyph first so it stays visible above the full gallery at 256x256.
    if (new_glyph) frame.text("Yeni glif: ğ");
    build_gallery(frame, state);
    auto finished = frame.finish();
    return success(finished);
}

std::shared_ptr<const wui::TextureSnapshot> atlas(const wui::DrawPacket& packet) {
    for (const auto& texture : packet->textures)
        if (texture->width == 1024 && texture->height == 1024) return texture;
    throw std::runtime_error("Gallery packet contains no real text atlas");
}
bool same_pixels(const wui::test::Image& a, const wui::test::Image& b) {
    return a.width == b.width && a.height == b.height &&
        a.pixels.size() == b.pixels.size() &&
        std::equal(a.pixels.begin(), a.pixels.end(), b.pixels.begin(),
            [](const wui::test::Pixel& x, const wui::test::Pixel& y) {
                return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
            });
}

} // namespace

TEST_CASE("Vulkan rerenders old real gallery atlas after glyph update and reconstruction") {
    NativeHarness native(256);
    auto textures = std::make_shared<wui::TextureStore>(64 * 1024 * 1024);
    const auto regular = font_bytes(std::filesystem::path(WUI_FONT_DIR) / "Inter-Regular.ttf");
    const auto semibold = font_bytes(std::filesystem::path(WUI_FONT_DIR) / "Inter-SemiBold.ttf");
    auto made = wui::make_text_service(regular, semibold, textures);
    auto service = success(made);
    wui::ContextDesc desc;
    desc.textures = textures;
    desc.text = service;
    auto context_result = wui::Context::create(std::move(desc));
    auto context = success(context_result);
    GalleryState state;
    prepare_gallery_model(state);
    auto icon = make_gallery_icon(*textures);
    state.icon = success(icon);
    const auto old = gallery_packet(*context, state, false);
    const auto original_atlas = atlas(old);
    const auto original_pixels = original_atlas->rgba;
    auto created = wui::vulkan::Renderer::create(native.config());
    auto renderer = success(created);
    const auto old_image = render_reference(native, *renderer, old);

    const auto newer = gallery_packet(*context, state, true);
    const auto updated_atlas = atlas(newer);
    CHECK(updated_atlas->handle == original_atlas->handle);
    CHECK(updated_atlas->revision > original_atlas->revision);
    CHECK(updated_atlas->rgba != original_pixels);
    CHECK(original_atlas->rgba == original_pixels);
    const auto new_image = render_reference(native, *renderer, newer);
    bool visible_update = false;
    REQUIRE(new_image.pixels.size() == old_image.pixels.size());
    for (std::size_t i = 0; i < old_image.pixels.size(); ++i) {
        const auto a = old_image.pixels[i], b = new_image.pixels[i];
        visible_update |= a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a;
    }
    CHECK(visible_update);
    const auto old_again = render_reference(native, *renderer, old);
    CHECK(same_pixels(old_image, old_again));
    CHECK_FALSE(renderer->shutdown().has_value());
    renderer.reset();
    auto reconstructed = wui::vulkan::Renderer::create(native.config());
    auto replacement = success(reconstructed);
    const auto rebuilt_old = render_reference(native, *replacement, old);
    CHECK(same_pixels(old_image, rebuilt_old));
    CHECK_FALSE(wui::test::write_ppm(old_image, "vulkan-gallery-old.ppm").has_value());
    CHECK_FALSE(wui::test::write_ppm(new_image, "vulkan-gallery-new-glyph.ppm").has_value());
    CHECK_FALSE(wui::test::write_ppm(rebuilt_old, "vulkan-gallery-rebuilt-old.ppm").has_value());
    CHECK_FALSE(replacement->shutdown().has_value());
    replacement.reset();
    native.cleanup();
    native.check_validation();
}
#endif
