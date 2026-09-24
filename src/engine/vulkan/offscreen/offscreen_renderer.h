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

namespace moleculens::engine {

struct RenderView;

enum class PixelFormat : uint8_t { Rgba8, Rgba16F };

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

    // 像素回读（vkCmdCopyImage → staging → CPU，主方案 §12.5 ④）
    bool readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt);

    void resize(uint32_t width, uint32_t height);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    VulkanDevice& device() { return *device_; }

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
    std::unique_ptr<VulkanDevice> ownedDevice_;  // headless 模式自建设备（共享模式为空）
    RenderConfig config_;
    uint32_t width_ = 1, height_ = 1;
    bool initialized_ = false;

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

    // 共享网格与每帧绘制状态（定义见 vulkan/draw/draw_geometry.h）
    SharedMeshes    meshes_;     // 球/柱单位网格（各离屏实例一份）
    FrameGpuBuffers gpu_;        // 每帧 GPU 缓冲（HOST_VISIBLE，帧内复用）
    DrawList        drawList_;   // 每帧 CPU 绘制列表（解码结果）

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
