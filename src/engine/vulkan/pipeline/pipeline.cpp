// =============================================================================
// MolecuLens · engine · vulkan/pipeline/pipeline.cpp —— 管线实现
// =============================================================================

#include "engine/vulkan/pipeline/pipeline.h"

#include <cstring>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

ShaderModule::~ShaderModule() { destroy(); }

bool ShaderModule::create(VulkanDevice& device, VkShaderStageFlagBits stage,
                          const uint8_t* spv, size_t spvSize) {
    destroy();
    device_ = &device;
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spvSize;
    info.pCode = reinterpret_cast<const uint32_t*>(spv);
    if (vkCreateShaderModule(device.device(), &info, nullptr, &module_) != VK_SUCCESS) {
        spdlog::error("[backend] vkCreateShaderModule failed");
        return false;
    }
    stageInfo_.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo_.stage = stage;
    stageInfo_.module = module_;
    stageInfo_.pName = "main";
    return true;
}

void ShaderModule::destroy() {
    if (module_ != VK_NULL_HANDLE && device_) {
        vkDestroyShaderModule(device_->device(), module_, nullptr);
    }
    module_ = VK_NULL_HANDLE;
}

void Pipeline::destroy(VulkanDevice& device) {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device.device(), pipeline_, nullptr);
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device.device(), layout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
}

bool PipelineBuilder::build(VulkanDevice& device, Pipeline* out) {
    // 管线布局
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(descriptorLayouts_.size());
    layoutInfo.pSetLayouts = descriptorLayouts_.data();
    if (pushRange_.size > 0) {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange_;
    }
    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(device.device(), &layoutInfo, nullptr, &layout) !=
        VK_SUCCESS) {
        spdlog::error("[backend] vkCreatePipelineLayout failed");
        return false;
    }

    // 顶点输入
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount =
        static_cast<uint32_t>(vertexBindings_.size());
    vertexInput.pVertexBindingDescriptions = vertexBindings_.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(vertexAttrs_.size());
    vertexInput.pVertexAttributeDescriptions = vertexAttrs_.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = topology_;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = polygonMode_;   // FILL 实心 / LINE 线框（fillModeNonSolid 特性已开）
    rasterizer.lineWidth = lineWidth_;
    rasterizer.cullMode = cullMode_;
    rasterizer.frontFace = frontFace_;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = samples_;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = depthTest_ ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = depthWrite_ ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = 0xF;
    blendAttachment.blendEnable = blendEnable_ ? VK_TRUE : VK_FALSE;
    if (blendEnable_) {
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<uint32_t>(stages_.size());
    info.pStages = stages_.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = layout;
    info.renderPass = renderPass_;
    info.subpass = 0;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(device.device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        spdlog::error("[backend] vkCreateGraphicsPipelines failed");
        vkDestroyPipelineLayout(device.device(), layout, nullptr);
        return false;
    }
    out->pipeline_ = pipeline;
    out->layout_ = layout;
    return true;
}

} // namespace moleculens::engine
