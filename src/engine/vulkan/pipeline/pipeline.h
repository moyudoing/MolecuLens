// =============================================================================
// MolecuLens · engine · vulkan/pipeline/pipeline.h —— 图形管线
// 规格来源：主方案 §6.3（sphere/cylinder/line/ribbon/surface/pick/imgui 管线；
//           着色器经 cmake 内嵌为 C++ 头；push constant = MaterialBlock 64B）
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "engine/vulkan/backend/draw_list.h"   // DrawMode（体系渲染模式）
#include "engine/vulkan/device/device.h"

namespace moleculens::engine {

// 着色器模块（从内嵌 SPIR-V 头创建）
class ShaderModule {
public:
    ShaderModule() = default;
    ~ShaderModule();
    bool create(VulkanDevice& device, VkShaderStageFlagBits stage,
                const uint8_t* spv, size_t spvSize);
    [[nodiscard]] VkShaderModule module() const { return module_; }
    [[nodiscard]] VkPipelineShaderStageCreateInfo stage() const { return stageInfo_; }
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

    [[nodiscard]] VkPipeline pipeline() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

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
    // 颜色附件数（默认 1）。深度-only pass（阴影贴图）必须传 **0**——
    // 与渲染通道的附件数不一致是管线创建失败/校验错误的直接原因。
    PipelineBuilder& colorAttachments(uint32_t count) {
        colorAttachments_ = count;
        return *this;
    }
    PipelineBuilder& cullMode(VkCullModeFlags mode) { cullMode_ = mode; return *this; }
    PipelineBuilder& frontFace(VkFrontFace face) { frontFace_ = face; return *this; }
    PipelineBuilder& lineWidth(float w) { lineWidth_ = w; return *this; }
    PipelineBuilder& polygonMode(VkPolygonMode m) { polygonMode_ = m; return *this; }  // 线框

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
    uint32_t colorAttachments_ = 1;
    bool blendEnable_ = false;
    bool depthTest_ = true;
    bool depthWrite_ = true;
    VkCullModeFlags cullMode_ = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    float lineWidth_ = 1.0f;
    VkPolygonMode polygonMode_ = VK_POLYGON_MODE_FILL;
};

// =============================================================================
// PipelineSet —— 标准管线集（10 条管线 + 其 10 个着色器模块的统一生命周期）
// =============================================================================
// 职责：把"引擎需要哪些管线、各自的顶点布局与状态"收敛到一处。
//
// 动机：重构前 VulkanRenderer::createPipelines 与 OffscreenRenderer::initCommon
//       各有一份 ~170 行的管线创建代码（逐行同构），且两个后端头文件各自声明
//       10 个 ShaderModule* 裸指针成员、shutdown 里各写一遍 new/delete。
//       任何着色器或顶点布局变更都要同步改两处。
//
// 采样数：renderPass 与 samples 由调用方给出并**必须与后端实际使用的渲染通道一致**。
//       离屏 FBO 附件恒为 1x，故离屏路径传 VK_SAMPLE_COUNT_1_BIT；实时用设备 MSAA。
//       采样数不匹配的管线与渲染通道不兼容 → 全屏片元不绘制（历史坑，见 moleculens-fixes）。
class PipelineSet {
public:
    PipelineSet() = default;
    ~PipelineSet() = default;
    PipelineSet(const PipelineSet&) = delete;
    PipelineSet& operator=(const PipelineSet&) = delete;

    // 创建全部管线与着色器模块；任一步失败返回 false（outError 给出失败管线名）。
    bool create(VulkanDevice& device, VkRenderPass renderPass,
                VkSampleCountFlagBits samples, VkDescriptorSetLayout globalLayout,
                std::string* outError = nullptr);

    // 销毁全部管线与着色器模块（须在 renderPass 销毁之前调用）。
    void destroy(VulkanDevice& device);

    // —— 按体系渲染模式取管线（DrawMode：0 实心 / 1 线框 / 2 透明 / 3 点）——
    // 点模式（Point）的映射按几何类型而定：
    //   球     → point（GL_POINTS，1 顶点/实例）
    //   键     → 实心降级（调用方已把点模式的键拆成线段走 line()）
    //   网格   → 实心降级（TriMesh 无点绘制语义）
    // 越界模式一律回退实心（等价于原先的 mode % 4 取模）。
    [[nodiscard]] const Pipeline& sphere(DrawMode mode) const;
    [[nodiscard]] const Pipeline& cylinder(DrawMode mode) const;
    [[nodiscard]] const Pipeline& mesh(DrawMode mode) const;
    // 线段管线（点模式的键也走此管线）
    [[nodiscard]] const Pipeline& line() const { return line_; }
    // GL_POINTS 管线
    [[nodiscard]] const Pipeline& point() const { return point_; }

private:
    // 着色器模块（值成员；生命周期与管线集一致）
    ShaderModule sphereVert_, sphereFrag_;
    ShaderModule cylinderVert_, cylinderFrag_;
    ShaderModule lineVert_, lineFrag_;
    ShaderModule meshVert_, meshFrag_;
    ShaderModule pointVert_, pointFrag_;

    // 球：实心 / 线框 / 透明
    Pipeline sphereSolid_, sphereWire_, sphereBlend_;
    // 键圆柱：实心 / 线框 / 透明
    Pipeline cylinderSolid_, cylinderWire_, cylinderBlend_;
    // 三角网格：实心 / 线框 / 透明
    Pipeline meshSolid_, meshWire_, meshBlend_;
    Pipeline line_;
    Pipeline point_;
};

} // namespace moleculens::engine
