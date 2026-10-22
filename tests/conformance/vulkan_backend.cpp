#include "conformance.hpp"

#include <wui/backends/vulkan.hpp>

#include "../../backends/renderer/vulkan_memory.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wui::conformance {
namespace {

constexpr std::uint32_t kFrames = 3;
constexpr std::uint64_t kWaitNs = 10'000'000'000ull;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

Error vk_error(const char* what, VkResult result) {
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed VkResult=%d", what, static_cast<int>(result));
    return Error{result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost : ErrorCode::backend_failure, text};
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& item : list)
        if (std::strcmp(item.extensionName, name) == 0) return true;
    return false;
}

std::vector<VkExtensionProperties> instance_extensions(const char* layer) {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> list(count);
    if (vkEnumerateInstanceExtensionProperties(layer, &count, list.data()) != VK_SUCCESS) return {};
    list.resize(count);
    return list;
}

// Host harness: owns instance, device, render pass, per-size targets, three
// frame slots (command buffer + fence) and readback. The renderer under test
// only sees the public wui::vulkan API, exactly as an application would use it.
class VulkanBackend final : public OffscreenBackend {
public:
    ~VulkanBackend() override {
        if (device_) vkDeviceWaitIdle(device_);
        destroy_renderer();
        destroy_device();
        if (messenger_) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance_, messenger_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }

    Status initialize(const Options&) {
        if (auto error = create_instance()) return error;
        if (auto error = select_physical_device()) return error;
        if (auto error = create_device()) return error;
        return create_renderer();
    }

    std::string_view name() const override { return "vulkan-hardware"; }
    std::string adapter_description() const override { return adapter_; }
    bool debug_layer() const override { return debug_; }

    Result<Image> render(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return *error;
        const std::uint32_t slot = next_slot();
        if (pending_[slot]) {
            // The host only reuses a slot after its fence completed and the renderer retired it.
            if (auto error = collect_all()) return *error;
        }
        if (auto error = begin(slot)) return *error;
        auto prepared = renderer_->prepare({slot, commands_[slot], fences_[slot]}, packet);
        if (auto* error = std::get_if<Error>(&prepared)) {
            discard(slot);
            return *error;
        }
        auto& ticket = std::get<vulkan::PreparedFrame>(prepared);
        begin_pass(slot, w, h);
        if (auto error = renderer_->record(ticket, target(w, h))) {
            discard(slot);
            renderer_->abandon(std::move(ticket));
            return *error;
        }
        vkCmdEndRenderPass(commands_[slot]);
        record_readback(slot, w, h);
        if (auto error = submit(slot)) {
            discard(slot);
            renderer_->abandon(std::move(ticket));
            return *error;
        }
        renderer_->submitted(std::move(ticket));
        pending_[slot] = true;
        if (auto error = collect_all()) return *error;
        return read_pixels(w, h);
    }

    Status submit_in_flight(const DrawPacket& packet, std::uint32_t w, std::uint32_t h) override {
        if (auto error = ensure_target(w, h)) return error;
        const std::uint32_t slot = next_slot();
        if (pending_[slot]) {
            // The host has not collected this slot: the renderer must refuse it
            // before touching the (possibly still executing) command buffer or fence.
            auto prepared = renderer_->prepare({slot, commands_[slot], fences_[slot]}, packet);
            if (auto* error = std::get_if<Error>(&prepared)) return *error;
            renderer_->abandon(std::move(std::get<vulkan::PreparedFrame>(prepared)));
            return Error{ErrorCode::backend_failure, "renderer accepted a frame slot that is still in flight"};
        }
        if (auto error = begin(slot)) return error;
        auto prepared = renderer_->prepare({slot, commands_[slot], fences_[slot]}, packet);
        if (auto* error = std::get_if<Error>(&prepared)) {
            discard(slot);
            return *error;
        }
        auto& ticket = std::get<vulkan::PreparedFrame>(prepared);
        begin_pass(slot, w, h);
        if (auto error = renderer_->record(ticket, target(w, h))) {
            discard(slot);
            renderer_->abandon(std::move(ticket));
            return error;
        }
        vkCmdEndRenderPass(commands_[slot]);
        if (auto error = submit(slot)) {
            discard(slot);
            renderer_->abandon(std::move(ticket));
            return error;
        }
        renderer_->submitted(std::move(ticket));
        pending_[slot] = true;
        return std::nullopt;
    }

    Status collect_all() override {
        for (std::uint32_t slot = 0; slot < kFrames; ++slot) {
            if (!pending_[slot]) continue;
            const VkResult waited = vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, kWaitNs);
            if (waited != VK_SUCCESS) return vk_error("vkWaitForFences", waited);
            pending_[slot] = false;
        }
        return renderer_->collect();
    }

    Status recreate() override {
        // Shutdown + create on a replacement VkDevice: every native resource,
        // including the atlas images, is rebuilt from the packets alone.
        if (auto error = collect_all()) return error;
        if (auto error = renderer_->shutdown()) return error;
        destroy_renderer();
        destroy_device();
        if (auto error = create_device()) return error;
        return create_renderer();
    }

    std::uint64_t debug_messages() override {
        const auto total = messages_;
        messages_ = 0;
        return total;
    }

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        if (!(severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) return VK_FALSE;
        const char* text = data && data->pMessage ? data->pMessage : "?";
        // The Vulkan loader reports machine-level manifest issues (e.g. a duplicate
        // third-party implicit layer) through the same messenger. They are not
        // produced by the renderer or the harness, so they are shown but not counted.
        if (data && data->pMessageIdName && std::strcmp(data->pMessageIdName, "Loader Message") == 0) {
            std::fprintf(stderr, "vulkan loader (not counted): %s\n", text);
            return VK_FALSE;
        }
        ++static_cast<VulkanBackend*>(user)->messages_;
        std::fprintf(stderr, "vulkan validation: %s\n", text);
        return VK_FALSE;
    }

    VkDebugUtilsMessengerCreateInfoEXT messenger_info() {
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = &on_message;
        info.pUserData = this;
        return info;
    }

    Status create_instance() {
        std::uint32_t version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_1)
            return Error{ErrorCode::unsupported, "Vulkan 1.1 loader required"};
        std::uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        bool layer = false;
        for (std::uint32_t i = 0; i < count; ++i)
            if (std::strcmp(layers[i].layerName, kValidationLayer) == 0) layer = true;
        const auto global = instance_extensions(nullptr);
        const bool debug_utils = has_extension(global, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const auto layer_ext = layer ? instance_extensions(kValidationLayer) : std::vector<VkExtensionProperties>{};
        const bool layer_settings = has_extension(layer_ext, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        const bool features = has_extension(layer_ext, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

        std::vector<const char*> extensions;
        if (debug_utils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const void* chain = nullptr;
        // Synchronization validation: VK_EXT_layer_settings (current), else VK_EXT_validation_features.
        const VkBool32 enable = VK_TRUE;
        VkLayerSettingEXT setting{kValidationLayer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enable};
        VkLayerSettingsCreateInfoEXT settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
        settings.settingCount = 1;
        settings.pSettings = &setting;
        const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount = 1;
        validation.pEnabledValidationFeatures = &sync;
        const char* sync_mechanism = "none";
        if (layer && layer_settings) {
            extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
            chain = &settings;
            sync_mechanism = "VK_EXT_layer_settings validate_sync";
        } else if (layer && features) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            chain = &validation;
            sync_mechanism = "VK_EXT_validation_features";
        }
        // Also catches messages emitted during vkCreateInstance/vkDestroyInstance.
        auto creation_messenger = messenger_info();
        if (debug_utils) {
            creation_messenger.pNext = chain;
            chain = &creation_messenger;
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "wui_conformance_vulkan";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pNext = chain;
        info.pApplicationInfo = &app;
        info.enabledLayerCount = layer ? 1u : 0u;
        info.ppEnabledLayerNames = &kValidationLayer;
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
        if (result != VK_SUCCESS) return vk_error("vkCreateInstance", result);
        if (debug_utils) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            const auto messenger = messenger_info();
            if (!create || create(instance_, &messenger, nullptr, &messenger_) != VK_SUCCESS)
                messenger_ = VK_NULL_HANDLE;
        }
        // Honest reporting: the run only counts as validated with the layer,
        // synchronization validation and a messenger all active.
        debug_ = layer && messenger_ != VK_NULL_HANDLE && chain != nullptr &&
            std::strcmp(sync_mechanism, "none") != 0;
        validation_ = std::string(layer ? kValidationLayer : "NO validation layer") +
            ", sync validation via " + sync_mechanism;
        return std::nullopt;
    }

    Status select_physical_device() {
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        VkPhysicalDevice fallback = VK_NULL_HANDLE;
        std::uint32_t fallback_family = 0;
        for (std::uint32_t d = 0; d < count; ++d) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices[d], &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1) continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, queues.data());
            for (std::uint32_t f = 0; f < families; ++f) {
                if (!(queues[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !queues[f].queueCount) continue;
                if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !physical_) {
                    physical_ = devices[d];
                    family_ = f;
                } else if (!fallback) {
                    fallback = devices[d];
                    fallback_family = f;
                }
                break;
            }
        }
        if (!physical_) {
            physical_ = fallback;
            family_ = fallback_family;
        }
        if (!physical_) return Error{ErrorCode::unsupported, "No Vulkan 1.1 device with a graphics queue"};
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        atom_ = properties.limits.nonCoherentAtomSize;
        char text[512];
        std::snprintf(text, sizeof(text),
            "%s (vendor 0x%04X device 0x%04X, api %u.%u.%u, driver 0x%08X, nonCoherentAtomSize %llu); %s; "
            "renderer force_explicit_flush=on",
            properties.deviceName, properties.vendorID, properties.deviceID,
            VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
            VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion,
            static_cast<unsigned long long>(atom_), validation_.c_str());
        adapter_ = text;
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
        return std::nullopt;
    }

    Status create_device() {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        VkResult result = vkCreateDevice(physical_, &info, nullptr, &device_);
        if (result != VK_SUCCESS) return vk_error("vkCreateDevice", result);
        vkGetDeviceQueue(device_, family_, 0, &queue_);

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        // Orders this pass's clear/writes after earlier submissions' attachment
        // writes and readback copies of the same image (frames in flight).
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 1;
        pass.pAttachments = &attachment;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &dependency;
        result = vkCreateRenderPass(device_, &pass, nullptr, &pass_);
        if (result != VK_SUCCESS) return vk_error("vkCreateRenderPass", result);

        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = family_;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        result = vkCreateCommandPool(device_, &pool, nullptr, &pool_);
        if (result != VK_SUCCESS) return vk_error("vkCreateCommandPool", result);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = kFrames;
        result = vkAllocateCommandBuffers(device_, &allocate, commands_.data());
        if (result != VK_SUCCESS) return vk_error("vkAllocateCommandBuffers", result);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (auto& item : fences_) {
            result = vkCreateFence(device_, &fence, nullptr, &item);
            if (result != VK_SUCCESS) return vk_error("vkCreateFence", result);
        }
        pending_ = {};
        cursor_ = 0;
        return std::nullopt;
    }

    Status create_renderer() {
        vulkan::Config config{physical_, device_, family_, pass_};
        config.max_frames = kFrames;
        // Runs the noncoherent flush path (atom-aligned ranges) under validation
        // even when the selected upload memory is host-coherent.
        config.force_explicit_flush = true;
        auto created = vulkan::Renderer::create(config);
        if (auto* error = std::get_if<Error>(&created)) return *error;
        renderer_ = std::get<std::unique_ptr<vulkan::Renderer>>(std::move(created));
        return std::nullopt;
    }

    void destroy_renderer() { renderer_.reset(); }

    void destroy_target() {
        if (!device_) return;
        if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        if (view_) vkDestroyImageView(device_, view_, nullptr);
        if (image_) vkDestroyImage(device_, image_, nullptr);
        if (image_memory_) vkFreeMemory(device_, image_memory_, nullptr);
        if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
        if (readback_memory_) vkFreeMemory(device_, readback_memory_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        image_memory_ = VK_NULL_HANDLE;
        readback_ = VK_NULL_HANDLE;
        readback_memory_ = VK_NULL_HANDLE;
        target_w_ = target_h_ = 0;
    }

    void destroy_device() {
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        destroy_target();
        for (auto& fence : fences_) {
            if (fence) vkDestroyFence(device_, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        if (pass_) vkDestroyRenderPass(device_, pass_, nullptr);
        pool_ = VK_NULL_HANDLE;
        pass_ = VK_NULL_HANDLE;
        commands_ = {};
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    std::optional<std::uint32_t> memory_type(std::uint32_t bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) const {
        std::optional<std::uint32_t> any;
        for (std::uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
            if (!(bits & (1u << i))) continue;
            const auto flags = memory_.memoryTypes[i].propertyFlags;
            if ((flags & required) != required) continue;
            if ((flags & preferred) == preferred) return i;
            if (!any) any = i;
        }
        return any;
    }

    Status ensure_target(std::uint32_t w, std::uint32_t h) {
        if (image_ && target_w_ == w && target_h_ == h) return std::nullopt;
        if (auto error = collect_all()) return error;
        destroy_target();
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {w, h, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkResult result = vkCreateImage(device_, &image, nullptr, &image_);
        if (result != VK_SUCCESS) return vk_error("vkCreateImage(target)", result);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, image_, &requirements);
        auto type = memory_type(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!type) return Error{ErrorCode::unsupported, "no memory type for the target image"};
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        result = vkAllocateMemory(device_, &allocation, nullptr, &image_memory_);
        if (result != VK_SUCCESS) return vk_error("vkAllocateMemory(target)", result);
        result = vkBindImageMemory(device_, image_, image_memory_, 0);
        if (result != VK_SUCCESS) return vk_error("vkBindImageMemory(target)", result);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device_, &view, nullptr, &view_);
        if (result != VK_SUCCESS) return vk_error("vkCreateImageView(target)", result);
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = pass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &view_;
        framebuffer.width = w;
        framebuffer.height = h;
        framebuffer.layers = 1;
        result = vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_);
        if (result != VK_SUCCESS) return vk_error("vkCreateFramebuffer", result);

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = static_cast<VkDeviceSize>(w) * h * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        result = vkCreateBuffer(device_, &buffer, nullptr, &readback_);
        if (result != VK_SUCCESS) return vk_error("vkCreateBuffer(readback)", result);
        vkGetBufferMemoryRequirements(device_, readback_, &requirements);
        type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (!type) return Error{ErrorCode::unsupported, "no host-visible readback memory"};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        readback_size_ = requirements.size;
        result = vkAllocateMemory(device_, &allocation, nullptr, &readback_memory_);
        if (result != VK_SUCCESS) return vk_error("vkAllocateMemory(readback)", result);
        result = vkBindBufferMemory(device_, readback_, readback_memory_, 0);
        if (result != VK_SUCCESS) return vk_error("vkBindBufferMemory(readback)", result);
        target_w_ = w;
        target_h_ = h;
        return std::nullopt;
    }

    std::uint32_t next_slot() {
        const std::uint32_t slot = cursor_;
        cursor_ = (cursor_ + 1) % kFrames;
        return slot;
    }

    vulkan::Target target(std::uint32_t w, std::uint32_t h) const { return {pass_, 0, {w, h}}; }

    Status begin(std::uint32_t slot) {
        VkResult result = vkResetFences(device_, 1, &fences_[slot]);
        if (result != VK_SUCCESS) return vk_error("vkResetFences", result);
        result = vkResetCommandBuffer(commands_[slot], 0);
        if (result != VK_SUCCESS) return vk_error("vkResetCommandBuffer", result);
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(commands_[slot], &info);
        if (result != VK_SUCCESS) return vk_error("vkBeginCommandBuffer", result);
        return std::nullopt;
    }

    // Host-side discard of an unsubmitted recording; the fence is re-signalled
    // by an empty submission so the slot returns to its idle (signalled) state.
    void discard(std::uint32_t slot) {
        vkResetCommandBuffer(commands_[slot], 0);
        vkQueueSubmit(queue_, 0, nullptr, fences_[slot]);
        vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, kWaitNs);
    }

    void begin_pass(std::uint32_t slot, std::uint32_t w, std::uint32_t h) {
        VkClearValue clear{};
        clear.color = {{kClear[0] / 255.0f, kClear[1] / 255.0f, kClear[2] / 255.0f, kClear[3] / 255.0f}};
        VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        info.renderPass = pass_;
        info.framebuffer = framebuffer_;
        info.renderArea.extent = {w, h};
        info.clearValueCount = 1;
        info.pClearValues = &clear;
        vkCmdBeginRenderPass(commands_[slot], &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    void record_readback(std::uint32_t slot, std::uint32_t w, std::uint32_t h) {
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = image_;
        image.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image.subresourceRange.levelCount = image.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commands_[slot], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(commands_[slot], image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &region);
        VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer.buffer = readback_;
        buffer.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commands_[slot], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
            nullptr, 1, &buffer, 0, nullptr);
    }

    Status submit(std::uint32_t slot) {
        VkResult result = vkEndCommandBuffer(commands_[slot]);
        if (result != VK_SUCCESS) return vk_error("vkEndCommandBuffer", result);
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &commands_[slot];
        result = vkQueueSubmit(queue_, 1, &info, fences_[slot]);
        if (result != VK_SUCCESS) return vk_error("vkQueueSubmit", result);
        return std::nullopt;
    }

    Result<Image> read_pixels(std::uint32_t w, std::uint32_t h) {
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device_, readback_memory_, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS) return vk_error("vkMapMemory(readback)", result);
        // Always invalidated (atom-aligned) so the noncoherent read path runs under validation.
        const auto range = vulkan::detail::atom_aligned_range(0, static_cast<std::uint64_t>(w) * h * 4,
            atom_, readback_size_);
        VkMappedMemoryRange invalidate{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        invalidate.memory = readback_memory_;
        invalidate.offset = range.offset;
        invalidate.size = range.size;
        result = vkInvalidateMappedMemoryRanges(device_, 1, &invalidate);
        if (result != VK_SUCCESS) {
            vkUnmapMemory(device_, readback_memory_);
            return vk_error("vkInvalidateMappedMemoryRanges", result);
        }
        Image image{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
        std::memcpy(image.rgba.data(), mapped, image.rgba.size());
        vkUnmapMemory(device_, readback_memory_);
        return image;
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    std::uint64_t atom_ = 1;
    std::uint32_t family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kFrames> commands_{};
    std::array<VkFence, kFrames> fences_{};
    std::array<bool, kFrames> pending_{};
    std::uint32_t cursor_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkDeviceSize readback_size_ = 0;
    std::uint32_t target_w_ = 0, target_h_ = 0;
    std::unique_ptr<vulkan::Renderer> renderer_;
    bool debug_ = false;
    std::string adapter_ = "unknown", validation_;
    std::uint64_t messages_ = 0;
};

}  // namespace

Result<std::unique_ptr<OffscreenBackend>> make_backend(const Options& options) {
    auto backend = std::make_unique<VulkanBackend>();
    if (auto error = backend->initialize(options)) return *error;
    return std::unique_ptr<OffscreenBackend>(std::move(backend));
}

}  // namespace wui::conformance
