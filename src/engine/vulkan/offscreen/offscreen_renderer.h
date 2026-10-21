// =============================================================================
// MolecuLens · engine · vulkan/offscreen/offscreen_renderer.h —— 离屏渲染
// 规格来源：主方案 §6.1/§12.5（截图/高 DPI 导出/动画导出/CLI 无头；
//           复用同一套命令流执行；readbackPixels RGBA8/16F）
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <volk.h>

#include "engine/cmd/cmd.h"
#include "engine/vulkan/backend/draw_list.h"
#include "engine/vulkan/desc/descriptors.h"
#include "engine/vulkan/device/device.h"
#include "engine/vulkan/draw/draw_geometry.h"
#include "engine/vulkan/memory/buffer.h"
#include "engine/vulkan/pipeline/pipeline.h"
#include "engine/vulkan/renderpass/render_pass.h"
#include "engine/vulkan/shadow/shadow_map.h"

namespace moleculens::engine {

struct RenderView;

enum class PixelFormat : uint8_t { Rgba8, Rgba16F };

// 颜色附件格式（主方案 §12.5）：
//   Rgba8Srgb   —— 默认。硬件在写入时做 linear→sRGB 编码，与主交换链
//                  （B8G8R8A8_SRGB）一致 → 导出图与视口观感相同。
//   Rgba16Float —— 半浮点线性附件：不经 8-bit 量化，且动态范围不受 [0,1] 限制。
//                  用于高位深/HDR 导出（回读后由调用方做色调映射 + sRGB 编码）。
enum class ColorAttachment : uint8_t { Rgba8Srgb, Rgba16Float };

// 离屏渲染：截图 / 高 DPI 导出 / 动画导出 / CLI 无头（无 Swapchain，主方案 §6.1）
class OffscreenRenderer {
public:
    OffscreenRenderer() = default;
    ~OffscreenRenderer();

    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;
    OffscreenRenderer(OffscreenRenderer&&) = delete;
    OffscreenRenderer& operator=(OffscreenRenderer&&) = delete;

    // 无窗口初始化（headless，主方案 §11.3 / CLI / GUI 视口 FBO §7.7）
    bool init(const RenderConfig& cfg, uint32_t width, uint32_t height,
              std::string* outError = nullptr);
    // 共享既有设备初始化（GUI 视口：离屏 FBO 建在主设备上，ImGui 描述符才能
    // 在同一设备内采样图像——跨设备 VkImageView 无法被主渲染通道采样）
    bool initShared(VulkanDevice& device, uint32_t width, uint32_t height,
                    std::string* outError = nullptr);
    void shutdown();

    // 执行命令流到离屏 FBO（MSAA resolve，主方案 §12.5 ③）
    void execute(const SceneCommandList& list, const RenderView& view);

    // 像素回读（vkCmdCopyImage → staging → CPU，主方案 §12.5 ④）。
    // fmt 必须与当前颜色附件一致（Rgba8 ↔ Rgba8Srgb，Rgba16F ↔ Rgba16Float）；
    // 不一致时返回 false（此前会按错误字节数拷贝出脏数据，见 .cpp 注释）。
    bool readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt);

    void resize(uint32_t width, uint32_t height);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // 网格质量档：球体细分 detail（1=80 面 … 4=5120 面）与圆柱段数。
    // 预览保持默认 3/24；导出前调用 setMeshQuality(4, 48) 可得到高倍放大仍圆的效果。
    void setMeshQuality(int detail, int segments);

    // 上一次 execute 实际提交的绘制调用数（批处理是否生效的观测口，见
    // draw_list.h 的 computeDrawRuns）。用于性能基准与调试，不影响渲染。
    [[nodiscard]] uint32_t lastDrawCalls() const { return gpu_.drawCalls; }

    // 设置颜色附件格式。格式变化会重建渲染通道 + 管线（管线引用渲染通道）
    // + FBO；设备不支持 16F 颜色附件时返回 false 并**保持原格式不变**。
    // 必须在 execute 之前调用；回读格式须与之一致（见 readbackPixels）。
    bool setColorAttachment(ColorAttachment fmt);
    [[nodiscard]] ColorAttachment colorAttachment() const { return attachment_; }

    VulkanDevice& device() { return *device_; }

    // 阴影贴图是否可用（设备/创建失败时为 false → 着色器按 lightParams.w == 0
    // 跳过查表，画面退化为"无阴影"而不是出错）
    [[nodiscard]] bool shadowMapAvailable() const { return shadow_.valid(); }

    // 颜色附件暴露（ImGui::Image 采样，主方案 §7.7：场景渲染进离屏 FBO）
    VkImageView colorView() const { return colorImage_.view(); }
    VkSampler sampler() const { return sampler_; }
    // 附件布局（execute 后为 SHADER_READ_ONLY_OPTIMAL，可供采样）
    VkImageLayout colorLayout() const { return colorLayout_; }
    // 清屏色（主方案 §4.4 背景）。alpha 用于透明背景导出：
    // 几何以不透明管线绘制（写自身 alpha），背景留 alpha=0 → PNG 可作合成素材。
    void setBackground(const float* rgb, float alpha = 1.0f);

private:
    bool createFramebuffer();
    void destroyFramebuffer();

    VulkanDevice* device_ = nullptr;             // 指向当前设备（自建或共享）
    int meshDetail_ = 3;                         // 球体细分档（3 = 1280 面）
    int meshSegments_ = 24;                      // 圆柱截面段数
    std::unique_ptr<VulkanDevice> ownedDevice_;  // headless 模式自建设备（共享模式为空）
    RenderConfig config_;
    uint32_t width_ = 1, height_ = 1;
    bool initialized_ = false;
    // 颜色附件格式。默认 **_SRGB** —— 与主交换链一致（见 createFramebuffer 注释：
    // UNORM 会让导出图比视口暗一档）。高动态范围导出可切到 16F 半浮点。
    ColorAttachment attachment_ = ColorAttachment::Rgba8Srgb;
    VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_SRGB;

    // init/initShared 共同的后段（FBO/管线/网格；device_ 已就绪）
    bool initCommon(std::string* outError);

    // 离屏附件
    Image colorImage_;     // 无 MSAA 或 resolve 目标
    Image colorMsaa_;      // MSAA 采样附件
    Image depthImage_;
    RenderPass renderPass_;
    Framebuffer framebuffer_;

    // 标准管线集（10 条管线 + 着色器模块；见 vulkan/pipeline/pipeline.h）
    PipelineSet pipelines_;
    DescriptorSetLayout globalLayout_;
    UboDescriptorSet globalUbo_;

    // 方向光阴影（各离屏实例一份；导出图与视口一样带阴影）
    ShadowMap       shadow_;

    // 共享网格与每帧绘制状态（定义见 vulkan/draw/draw_geometry.h）
    SharedMeshes    meshes_;     // 球/柱单位网格（各离屏实例一份）
    // 低细节共享网格（轨迹播放/拖时间轴时使用：球 320 面、柱 12 段）。播放时每帧
    // 要重画数千个原子，高细节网格在集显上会明显掉帧；暂停后自动回 hi 档。
    SharedMeshes    meshesLow_;
    FrameGpuBuffers gpu_;        // 每帧 GPU 缓冲（HOST_VISIBLE，帧内复用）
    DrawList        drawList_;   // 每帧 CPU 绘制列表（解码结果）
    // 解码缓存键：命令流对象 + serial（同 VulkanRenderer，见其头注释）
    const SceneCommandList* decodedList_ = nullptr;
    uint64_t          decodedRevision_ = UINT64_MAX;

    // 回读
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkDeviceSize readbackSize_ = 0;
    std::vector<uint8_t> pixels_;     // 最近一次 readback 结果

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImageLayout colorLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    float clearColor_[4] = {0.05f, 0.05f, 0.08f, 1.0f};
};

} // namespace moleculens::engine
