// =============================================================================
// MolecuLens · engine · vulkan/pipeline/pipeline.cpp —— 管线实现
// =============================================================================

#include "engine/vulkan/pipeline/pipeline.h"

#include <cstddef>
#include <cstring>

#include "spdlog/spdlog.h"

// 内嵌 SPIR-V 生成头（cmake/shaders.cmake；仅本 TU 需要，不进任何公共头）
#include "gen/shaders/sphere_vert.h"
#include "gen/shaders/sphere_frag.h"
#include "gen/shaders/cylinder_vert.h"
#include "gen/shaders/cylinder_frag.h"
#include "gen/shaders/line_vert.h"
#include "gen/shaders/line_frag.h"
#include "gen/shaders/mesh_vert.h"
#include "gen/shaders/mesh_frag.h"
#include "gen/shaders/point_vert.h"
#include "gen/shaders/point_frag.h"

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
    // 深度-only pass（阴影贴图）附件数为 0 → pAttachments 必须为 nullptr
    colorBlend.attachmentCount = colorAttachments_;
    colorBlend.pAttachments = colorAttachments_ > 0 ? &blendAttachment : nullptr;

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

// =============================================================================
// PipelineSet
// =============================================================================

namespace {

// 一条管线的描述（顶点布局 + 状态 + 着色器对）
struct PipeSpec {
    const VkPipelineShaderStageCreateInfo* stages = nullptr;   // vert / frag
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool blend = false;
    bool depthWrite = true;
    VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
    const char* name = "pipeline";
    // 剔除模式：默认双面不剔除；透明网格改用 BACK（见 mesh-blend 注释）。
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
};

} // namespace

bool PipelineSet::create(VulkanDevice& device, VkRenderPass renderPass,
                         VkSampleCountFlagBits samples, VkDescriptorSetLayout globalLayout,
                         std::string* outError) {
    auto loadShader = [&](ShaderModule& out, VkShaderStageFlagBits stage,
                          const uint8_t* spv, size_t size) -> bool {
        return out.create(device, stage, spv, size);
    };
    if (!loadShader(sphereVert_, VK_SHADER_STAGE_VERTEX_BIT, sphere_vert, sphere_vert_len) ||
        !loadShader(sphereFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, sphere_frag, sphere_frag_len) ||
        !loadShader(cylinderVert_, VK_SHADER_STAGE_VERTEX_BIT, cylinder_vert, cylinder_vert_len) ||
        !loadShader(cylinderFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, cylinder_frag, cylinder_frag_len) ||
        !loadShader(lineVert_, VK_SHADER_STAGE_VERTEX_BIT, line_vert, line_vert_len) ||
        !loadShader(lineFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, line_frag, line_frag_len) ||
        !loadShader(meshVert_, VK_SHADER_STAGE_VERTEX_BIT, mesh_vert, mesh_vert_len) ||
        !loadShader(meshFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, mesh_frag, mesh_frag_len) ||
        !loadShader(pointVert_, VK_SHADER_STAGE_VERTEX_BIT, point_vert, point_vert_len) ||
        !loadShader(pointFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, point_frag, point_frag_len)) {
        if (outError) *outError = "shader module creation failed";
        return false;
    }

    auto buildPipe = [&](const PipeSpec& s, Pipeline* out) -> bool {
        PipelineBuilder pb;
        pb.renderPass(renderPass).samples(samples).topology(s.topology)
            .shaders(s.stages, 2)
            .vertexInput(s.bindings, s.attrs)
            .descriptorLayouts({globalLayout})
            .pushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MaterialBlock))
            .depthTest(true).depthWrite(s.depthWrite).blend(s.blend)
            .polygonMode(s.polygon).cullMode(s.cull);
        if (!pb.build(device, out)) {
            if (outError) *outError = std::string(s.name) + " pipeline build failed";
            return false;
        }
        return true;
    };

    // —— 球（SphereInst 实例化）：实心 / 线框 / 透明 ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {sphereVert_.stage(),
                                                          sphereFrag_.stage()};
        const VkVertexInputBindingDescription bindings[] = {
            {0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(SphereInst), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, posRadius)},
            {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, color)},
            {3, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pickId)},
            {4, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pad)},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 2);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 5);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "sphere"}, &sphereSolid_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE, "sphere-wire"}, &sphereWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "sphere-blend"}, &sphereBlend_)) {
            return false;
        }
    }

    // —— 键圆柱（CylinderInst）：实心 / 线框 / 透明 ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {cylinderVert_.stage(),
                                                          cylinderFrag_.stage()};
        const VkVertexInputBindingDescription bindings[] = {
            {0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(CylinderInst), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, aPos)},
            {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, bPos)},
            {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, radius)},
            {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, color)},
            {5, 1, VK_FORMAT_R32G32_UINT, offsetof(CylinderInst, pickId_pad)},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 2);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 6);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "cylinder"}, &cylinderSolid_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE, "cylinder-wire"}, &cylinderWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "cylinder-blend"}, &cylinderBlend_)) {
            return false;
        }
    }

    // —— 线段（LineInst；点模式的键也走此管线）——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {lineVert_.stage(),
                                                          lineFrag_.stage()};
        // 顶点布局 = pos(float4) + color(float4) 交错 = 32B/顶点（line.vert）；
        // 曾用 48B stride 与数据不匹配 → 取数错位 → 线框渲染错乱。
        const VkVertexInputBindingDescription bindings[] = {
            {0, 32, VK_VERTEX_INPUT_RATE_VERTEX},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 1);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 2);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "line"}, &line_)) {
            return false;
        }
    }

    // —— 三角网格（TriMeshVertex 48B）：实心 / 线框 / 透明 ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {meshVert_.stage(),
                                                          meshFrag_.stage()};
        const VkVertexInputBindingDescription bindings[] = {
            {0, sizeof(TriMeshVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TriMeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TriMeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TriMeshVertex, color)},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 1);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 3);
        // 透明网格：只画**正面**（BACK 剔除）。原因：混合管线不写深度、且旧的
        // cull=NONE 会把背面也画一遍 —— 闭合等值面（ESP 表面/QuickSurf）的前后
        // 两层三角形按命令流顺序交错混合，出现沿网格排列的细亮线/方块伪影
        // （半透明表面上尤其明显）。只画正面后，透过表面看到的是不透明通道里
        // 已画好的球棍/背景，视觉效果正确且无排序伪影。
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "mesh"}, &meshSolid_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE, "mesh-wire"}, &meshWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "mesh-blend", VK_CULL_MODE_BACK_BIT},
                       &meshBlend_)) {
            return false;
        }
    }

    // —— 点（GL_POINTS；SphereInst 实例数据，无顶点绑定）——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {pointVert_.stage(),
                                                          pointFrag_.stage()};
        const VkVertexInputBindingDescription bindings[] = {
            {1, sizeof(SphereInst), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, posRadius)},
            {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, color)},
            {3, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pickId)},
            {4, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pad)},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 1);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 4);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "point"}, &point_)) {
            return false;
        }
    }
    return true;
}

void PipelineSet::destroy(VulkanDevice& device) {
    sphereSolid_.destroy(device);
    sphereWire_.destroy(device);
    sphereBlend_.destroy(device);
    cylinderSolid_.destroy(device);
    cylinderWire_.destroy(device);
    cylinderBlend_.destroy(device);
    meshSolid_.destroy(device);
    meshWire_.destroy(device);
    meshBlend_.destroy(device);
    line_.destroy(device);
    point_.destroy(device);
    sphereVert_.destroy();
    sphereFrag_.destroy();
    cylinderVert_.destroy();
    cylinderFrag_.destroy();
    lineVert_.destroy();
    lineFrag_.destroy();
    meshVert_.destroy();
    meshFrag_.destroy();
    pointVert_.destroy();
    pointFrag_.destroy();
}

const Pipeline& PipelineSet::sphere(DrawMode mode) const {
    switch (mode) {
        case DrawMode::Wireframe:   return sphereWire_;
        case DrawMode::Transparent: return sphereBlend_;
        case DrawMode::Point:       return point_;
        default:                    return sphereSolid_;
    }
}

const Pipeline& PipelineSet::cylinder(DrawMode mode) const {
    switch (mode) {
        case DrawMode::Wireframe:   return cylinderWire_;
        case DrawMode::Transparent: return cylinderBlend_;
        default:                    return cylinderSolid_;   // 含点模式降级
    }
}

const Pipeline& PipelineSet::mesh(DrawMode mode) const {
    switch (mode) {
        case DrawMode::Wireframe:   return meshWire_;
        case DrawMode::Transparent: return meshBlend_;
        default:                    return meshSolid_;       // 含点模式降级
    }
}

} // namespace moleculens::engine
