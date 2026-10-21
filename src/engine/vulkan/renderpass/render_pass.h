// =============================================================================
// MolecuLens · engine · vulkan/renderpass/render_pass.h —— 渲染通道
// 规格来源：主方案 §6.3/§6.5（color B8G8R8A8_SRGB + depth D32_SFLOAT +
//           MSAA resolve；离屏颜色/ID Pass 复用同一结构）
// =============================================================================

#pragma once

#include <vector>

#include <volk.h>

#include "engine/vulkan/device/device.h"

namespace moleculens::engine {

class VulkanDevice;

class RenderPass {
public:
    RenderPass() = default;
    ~RenderPass();

    // 窗口主通道（MSAA resolve）
    bool createMain(VulkanDevice& device, VkFormat colorFormat, VkFormat depthFormat,
                    VkSampleCountFlagBits samples);
    // 离屏通道（含 depth；无 MSAA resolve；可采样输出；finalLayout 供 readback/采样）
    bool createOffscreen(VulkanDevice& device, VkFormat colorFormat,
                         VkFormat depthFormat,
                         VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                         VkImageLayout finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // 拾取 ID Pass（R32_UINT，无深度，主方案 §5.6）
    bool createPickPass(VulkanDevice& device, VkFormat idFormat);
    // 阴影深度 pass（主方案 §6.6.4）：**只有深度附件、无颜色附件**，
    // 结束后深度图进入 shader 只读布局供主 pass 采样。
    // initialLayout 用 UNDEFINED：每帧都会 clear 深度，无需保留上一帧内容，
    // 且对首帧（图像尚未写过）也是合法转换。
    bool createShadowDepth(VulkanDevice& device, VkFormat depthFormat,
                           VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

    VkRenderPass renderPass() const { return renderPass_; }
    bool valid() const { return renderPass_ != VK_NULL_HANDLE; }
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
};

// 帧缓冲（窗口交换链视图或离屏附件）
class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    bool create(VulkanDevice& device, VkRenderPass renderPass,
                const std::vector<VkImageView>& attachments, VkExtent2D extent);
    VkFramebuffer framebuffer() const { return framebuffer_; }
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
};

} // namespace moleculens::engine
