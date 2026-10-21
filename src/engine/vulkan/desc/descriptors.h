// =============================================================================
// MolecuLens · engine · vulkan/desc/descriptors.h —— 描述符
// 规格来源：主方案 §6.3（全局 UBO：相机 + 光照 + 场景属性，set 0 binding 0）
// =============================================================================

#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <volk.h>

#include "engine/render/render_parameter.h"   // LightParams
#include "engine/vulkan/device/device.h"

namespace moleculens::engine {

// 全局 UBO 布局（与 shaders/global_ubo.glsl 对应，352B）
// 光照 = 半球环境光（lightDir/lightColor 槽：天顶色/地面色）+ 方向光（末尾新增槽）。
// 前 160B 的槽位语义与偏移**保持不变**（既有着色器/布局不受影响）；方向光字段
// 一律追加在末尾，避免破坏已定稿的字段布局（主方案 §6.6.4）。
struct GlobalUBO {
    glm::mat4 viewProj;        // 0-64
    glm::vec4 cameraPos;       // 64
    glm::vec4 lightDir;        // 80（半球光：rgb = 天顶色）
    glm::vec4 lightColor;      // 96（半球光：rgb = 地面色）
    glm::vec4 ambient;         // 112（rgb = 基础环境色, w = 半球光强度）
    glm::vec4 fog;             // 128（x near, y far, z mode, w density）
    glm::vec4 sceneProps;      // 144（x 全局缩放，yzw 保留）
    // —— 方向光（末尾追加；主方案 §6.6.4 方向光 + 阴影）——
    glm::vec4 dirLightDir;     // 160（xyz = 方向，指向光源，已归一化；w = 启用 0/1）
    glm::vec4 dirLightColor;   // 176（rgb = 颜色，w = 强度）
    glm::vec4 lightParams;     // 192（x 环境权重, y 漫反射权重, z 高光权重, w = 阴影强度）
    // 光源视图投影矩阵（深度 pass 用；按场景包围球拟合的正交投影，见
    // Renderer::sceneLighting）。mat4 需 16B 对齐 → 必须放在 192+16=208 处。
    glm::mat4 lightViewProj;   // 208-272
    // 阴影贴图尺寸（浮点存，供着色器做纹素级 PCF 偏移）+ 阴影开关已在 lightParams.w
    glm::vec4 shadowParams;    // 272（x = 阴影贴图边长, y = 深度偏移, zw 保留）
    // —— 渲染风格（v2.2；末尾追加，前 288B 语义不变）——
    // x = 漫反射环绕量, y = 掠射亮边, z = 掠射压暗(描边), w = 距离淡出倍率
    glm::vec4 styleParams;     // 288
    // 背景色（线性 RGB；雾/距离淡出的目标色，必须与实际清屏色一致）
    glm::vec4 backgroundColor; // 304
    // —— 剖切面（v2.3；末尾追加）——
    // clipPlane = (n.x, n.y, n.z, d)，平面方程 dot(p,n)+d=0；保留 <=0 一侧。
    glm::vec4 clipPlane;       // 320
    // x = 是否启用（0/1），yzw 保留
    glm::vec4 clipParams;      // 336
};
static_assert(sizeof(GlobalUBO) == 352);

// 填装 GlobalUBO 的光照字段（半球环境光 → 既有槽；方向光 → 末尾新增槽）。
// 实时与离屏两个后端共用同一实现——此前两处各写一份 UBO 构造，任何字段新增
// 都要改两遍，漏一处即布局/语义漂移。
void fillGlobalLighting(GlobalUBO& ubo, const LightParams& light);

// 填装风格 + 背景槽（与 fillGlobalLighting 分开：背景来自后端清屏值，光照来自
// 场景属性，来源不同；但两个后端都必须在更新 UBO 时调用一次）。
void fillRenderStyle(GlobalUBO& ubo, const RenderStyle& style,
                     const glm::vec3& backgroundColor);

// 填装剖切面槽（平面方程 + 启用位）。不改变管线/命令流。
void fillClipPlane(GlobalUBO& ubo, const glm::vec4& plane, bool enabled);

class DescriptorSetLayout {
public:
    DescriptorSetLayout() = default;
    ~DescriptorSetLayout();
    bool create(VulkanDevice& device, const std::vector<VkDescriptorSetLayoutBinding>&);
    VkDescriptorSetLayout layout() const { return layout_; }
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

// UBO 描述符池 + 每帧 set（环形 kMaxFramesInFlight）
//
// 除 binding 0 的全局 UBO 外，可选在 **binding 1** 绑定一张阴影贴图
// （combined image sampler）。阴影贴图的 view/sampler 在生命周期内稳定，故只在
// create 时写一次描述符，不必每帧重写。
//
// ⚠ 即使运行时不用阴影也必须绑定一张有效图（哪怕 1×1 占位）：着色器里 binding 1
//   是**静态使用**的（在 lightParams.w > 0 的分支内），未写入的描述符会触发校验错误。
class UboDescriptorSet {
public:
    UboDescriptorSet() = default;
    ~UboDescriptorSet();
    bool create(VulkanDevice& device, VkDescriptorSetLayout layout,
                VkDeviceSize bufferSize, VkImageView shadowView = VK_NULL_HANDLE,
                VkSampler shadowSampler = VK_NULL_HANDLE);
    void update(uint32_t frameIndex, const GlobalUBO& data);
    VkDescriptorSet set(uint32_t frameIndex) const {
        return frameIndex < sets_.size() ? sets_[frameIndex] : VK_NULL_HANDLE;
    }
    VkBuffer buffer(uint32_t frameIndex) const;
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
    std::vector<VkBuffer> buffers_;
    std::vector<VkDeviceMemory> memories_;
    VkDeviceSize bufferSize_ = 0;
};

} // namespace moleculens::engine
