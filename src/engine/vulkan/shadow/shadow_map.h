// =============================================================================
// MolecuLens · engine · vulkan/shadow/shadow_map.h —— 方向光阴影贴图
// 规格来源：主方案 §6.6.4 光照（方向光 + 阴影）；实现组织参考 Khronos
//           Vulkan-Samples 的 shadow mapping（设计参考，非依赖）
//
// 两遍结构：
//   ① 深度 pass：从**光源视角**把球/柱几何画进深度图（正交投影，按场景包围球
//      拟合；矩阵由 Renderer::sceneLighting 计算，见 GlobalUBO::lightViewProj）
//   ② 主 pass：片元用光矩阵把自己投到光源裁剪空间，与深度图比较 → 本影/半影
//
// 本模块**与主渲染目标无关**（自带渲染通道/帧缓冲/管线），故实时后端与离屏后端
// 共用同一份实现，不会出现"两处各写一份阴影代码"的漂移。
//
// ⚠ 覆盖范围：目前只投影**球与圆柱**（spacefill/CPK/球棍/棍状/甘草棍等主力表示）。
//    TriMesh（表面/卡通/QuickSurf）不投影 —— 它们的顶点缓冲由主 pass 的合并逻辑
//    产出，纳入本模块需要重构 draw_geometry 的上传职责，风险与收益不成比例。
//    未投影的表示会"没有影子"，开阴影时请注意这一点。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

#include <volk.h>

#include "engine/vulkan/backend/draw_list.h"
#include "engine/vulkan/desc/descriptors.h"   // GlobalUBO / UboDescriptorSet
#include "engine/vulkan/draw/draw_geometry.h"   // SharedMeshes（单位球/柱网格）
#include "engine/vulkan/memory/buffer.h"
#include "engine/vulkan/pipeline/pipeline.h"
#include "engine/vulkan/renderpass/render_pass.h"

namespace moleculens::engine {

class VulkanDevice;

class ShadowMap {
public:
    ShadowMap() = default;
    ~ShadowMap();
    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // 创建深度图 + 渲染通道 + 帧缓冲 + 深度-only 管线 + 光源 UBO 环。
    //   size         = 贴图边长（2 的幂；≤0 表示禁用，create 返回 false）
    //   globalLayout = 全局 UBO 描述符集布局（顶点着色器要用它读 viewProj）
    bool create(VulkanDevice& device, uint32_t size, VkDescriptorSetLayout globalLayout,
                std::string* outError = nullptr);
    void destroy();
    [[nodiscard]] bool valid() const { return initialized_; }

    // 采样源（主 pass 的 frag 用 set 0 binding 1 采样）
    [[nodiscard]] VkImageView view() const { return depth_.view(); }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t size() const { return size_; }

    // 记录深度 pass（调用方保证 cmd 处于渲染通道之外、且在提交之前调用）。
    //   lightUbo   = 以 lightViewProj 作为 viewProj 的全局 UBO；**其余字段必须与
    //                主 pass 相同**（尤其 sceneProps.x 全局缩放，否则深度几何与
    //                主几何错位 → 阴影整体偏移）
    //   frameIndex = UBO 环槽位（与主 UBO 的取槽规则一致）
    //   listRevision = 绘制列表修订号：用于判断本模块自己的实例缓冲是否需重传
    //   meshes     = 后端的共享单位球/柱网格（深度 pass 必须用与主 pass 相同的网格，
    //                否则同一实例数据会得到不同几何 → 阴影与实体错位）
    void record(VkCommandBuffer cmd, VulkanDevice& device, const DrawList& draw,
                const SharedMeshes& meshes, const GlobalUBO& lightUbo,
                uint32_t frameIndex, uint64_t listRevision);

private:
    bool createPipelines(VulkanDevice& device, std::string* outError);
    // 上传本模块自己的实例缓冲（球/柱）
    void uploadInstances(VulkanDevice& device, const DrawList& draw, uint64_t revision);

    VulkanDevice* device_ = nullptr;
    uint32_t size_ = 0;
    // 全局 UBO 描述符集布局（深度 pass 的管线布局需要它；不拥有其生命周期）
    VkDescriptorSetLayout globalLayout_ = VK_NULL_HANDLE;

    Image depth_;
    RenderPass pass_;
    Framebuffer fbo_;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // 深度 pass 的着色器：顶点阶段复用球/柱的**同一批**（几何必须与主 pass 一致），
    // 片元阶段为空（无颜色输出）
    ShaderModule sphereVert_, cylinderVert_, shadowFrag_;
    Pipeline spherePipe_, cylinderPipe_;

    UboDescriptorSet lightUbo_;

    // 本模块**自有**的实例缓冲。刻意不复用主 pass 的 FrameGpuBuffers：
    //   主 pass 的点模式会把键过滤成子集后上传，而 drawRun 的 firstInstance 是按
    //   那个子集的下标算的 —— 阴影 pass 若复用同一缓冲却按全量下标绘制，键会错位。
    //   自有一份的代价是每帧多传一次实例数据（6452 原子约 0.8 MB，可忽略），
    //   换来的是与主 pass 的上传簿记彻底解耦。
    Buffer sphereInst_, cylinderInst_;
    uint64_t instanceRevision_ = UINT64_MAX;
    uint32_t sphereCount_ = 0, cylinderCount_ = 0;

    bool initialized_ = false;
};

} // namespace moleculens::engine
