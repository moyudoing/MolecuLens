// =============================================================================
// MolecuLens · engine · vulkan/pipeline/pipeline.h —— 图形管线
// 规格来源：主方案 §6.3（sphere/cylinder/line/ribbon/surface/pick/imgui 管线；
//           着色器经 cmake 内嵌为 C++ 头；push constant = MaterialBlock 64B）
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <volk.h>

#include "engine/vulkan/device/device.h"

namespace moleculens::engine {

// 着色器模块（从内嵌 SPIR-V 头创建）
class ShaderModule {
public:
    ShaderModule() = default;
    ~ShaderModule();
    bool create(VulkanDevice& device, VkShaderStageFlagBits stage,
                const uint8_t* spv, size_t spvSize);
    VkShaderModule module() const { return module_; }
    VkPipelineShaderStageCreateInfo stage() const { return stageInfo_; }
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stageInfo_{};
};

// 管线对象（含 layout；销毁由 Renderer 统一管理）
class Pipeline {
public:
    Pipeline() = default;
    ~Pipeline() = default;

    VkPipeline pipeline() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    void destroy(VulkanDevice& device);

private:
    friend class PipelineBuilder;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

// 构建器
class PipelineBuilder {
public:
    PipelineBuilder() = default;

    PipelineBuilder& renderPass(VkRenderPass rp) { renderPass_ = rp; return *this; }
    PipelineBuilder& samples(VkSampleCountFlagBits s) { samples_ = s; return *this; }
    PipelineBuilder& topology(VkPrimitiveTopology t) { topology_ = t; return *this; }
    PipelineBuilder& shaders(const VkPipelineShaderStageCreateInfo* stages,
                             uint32_t count) {
        stages_.assign(stages, stages + count);
        return *this;
    }
    // 顶点输入（binding/attribute 列表）
    PipelineBuilder& vertexInput(const std::vector<VkVertexInputBindingDescription>& bindings,
                                 const std::vector<VkVertexInputAttributeDescription>& attrs) {
        vertexBindings_ = bindings;
        vertexAttrs_ = attrs;
        return *this;
    }
    // 描述符集布局（可选）
    PipelineBuilder& descriptorLayouts(const std::vector<VkDescriptorSetLayout>& layouts) {
        descriptorLayouts_ = layouts;
        return *this;
    }
    // push constant 范围（MaterialBlock 64B）
    PipelineBuilder& pushConstants(VkShaderStageFlags stages, uint32_t size) {
        pushRange_.stageFlags = stages;
        pushRange_.offset = 0;
        pushRange_.size = size;
        return *this;
    }
    // 深度/混合/光栅化状态
    PipelineBuilder& blend(bool enable) { blendEnable_ = enable; return *this; }
    PipelineBuilder& depthTest(bool enable) { depthTest_ = enable; return *this; }
    PipelineBuilder& depthWrite(bool enable) { depthWrite_ = enable; return *this; }
    PipelineBuilder& cullMode(VkCullModeFlags mode) { cullMode_ = mode; return *this; }
    PipelineBuilder& frontFace(VkFrontFace face) { frontFace_ = face; return *this; }
    PipelineBuilder& lineWidth(float w) { lineWidth_ = w; return *this; }

    bool build(VulkanDevice& device, Pipeline* out);
    void destroyAll(VulkanDevice& device);

private:
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    std::vector<VkPipelineShaderStageCreateInfo> stages_;
    std::vector<VkVertexInputBindingDescription> vertexBindings_;
    std::vector<VkVertexInputAttributeDescription> vertexAttrs_;
    std::vector<VkDescriptorSetLayout> descriptorLayouts_;
    VkPushConstantRange pushRange_{};
    bool blendEnable_ = false;
    bool depthTest_ = true;
    bool depthWrite_ = true;
    VkCullModeFlags cullMode_ = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    float lineWidth_ = 1.0f;
};

} // namespace moleculens::engine
