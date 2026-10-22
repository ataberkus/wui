#include "vulkan_pipeline.hpp"

#include "wui_vulkan_vert.hpp"
#include "wui_vulkan_frag.hpp"

#include <wui/draw.hpp>

#include <array>
#include <cstddef>

namespace wui::vulkan::detail {
namespace {
Status failed(VkResult result, const char* operation) {
    return Error{result == VK_ERROR_DEVICE_LOST ? ErrorCode::device_lost :
        ErrorCode::backend_failure, operation};
}
} // namespace

void destroy_pipeline(VkDevice device, Pipeline& pipeline) noexcept {
    if (pipeline.graphics) vkDestroyPipeline(device, pipeline.graphics, nullptr);
    if (pipeline.layout) vkDestroyPipelineLayout(device, pipeline.layout, nullptr);
    pipeline = {};
}

Status create_pipeline(const Config& config, VkDescriptorSetLayout texture_layout,
    Pipeline& output) {
    static_assert(sizeof(Vertex) == 32);
    static_assert(offsetof(Vertex, position) == 0);
    static_assert(offsetof(Vertex, uv) == 8);
    static_assert(offsetof(Vertex, premultiplied) == 16);

    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = sizeof(kVertexSpirv);
    module.pCode = kVertexSpirv;
    VkResult result = vkCreateShaderModule(config.device, &module, nullptr, &vertex);
    if (result != VK_SUCCESS) return failed(result, "Create Vulkan vertex shader");
    module.codeSize = sizeof(kFragmentSpirv);
    module.pCode = kFragmentSpirv;
    result = vkCreateShaderModule(config.device, &module, nullptr, &fragment);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(config.device, vertex, nullptr);
        return failed(result, "Create Vulkan fragment shader");
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &texture_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    Pipeline pipeline;
    result = vkCreatePipelineLayout(config.device, &layout, nullptr, &pipeline.layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(config.device, fragment, nullptr);
        vkDestroyShaderModule(config.device, vertex, nullptr);
        return failed(result, "Create Vulkan pipeline layout");
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;

    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, premultiplied)}
    }};
    VkPipelineVertexInputStateCreateInfo inputs{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    inputs.vertexBindingDescriptionCount = 1;
    inputs.pVertexBindingDescriptions = &binding;
    inputs.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    inputs.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = config.samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &inputs;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = pipeline.layout;
    info.renderPass = config.render_pass;
    info.subpass = config.subpass;
    result = vkCreateGraphicsPipelines(config.device, VK_NULL_HANDLE, 1, &info,
        nullptr, &pipeline.graphics);
    vkDestroyShaderModule(config.device, fragment, nullptr);
    vkDestroyShaderModule(config.device, vertex, nullptr);
    if (result != VK_SUCCESS) {
        destroy_pipeline(config.device, pipeline);
        return failed(result, "Create Vulkan graphics pipeline");
    }
    output = pipeline;
    return std::nullopt;
}

} // namespace wui::vulkan::detail
