// =============================================================================
// MolecuLens · engine · vulkan/renderpass/render_pass.cpp —— 渲染通道实现
// 规格来源：主方案 §6.3/§6.5
// =============================================================================

#include "engine/vulkan/renderpass/render_pass.h"

#include "spdlog/spdlog.h"

namespace moleculens::engine {

RenderPass::~RenderPass() { destroy(); }

bool RenderPass::createMain(VulkanDevice& device, VkFormat colorFormat,
                            VkFormat depthFormat, VkSampleCountFlagBits samples) {
    destroy();
    device_ = &device;

    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    // 颜色附件：MSAA 采样（resolve 源）
    VkAttachmentDescription color{};
    color.format = colorFormat;
    color.samples = samples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                         : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription resolve{};
    resolve.format = colorFormat;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = depthFormat;
    depth.samples = samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    std::vector<VkAttachmentDescription> attachments = {color, depth};
    if (msaa) {
        attachments.push_back(resolve);
    }

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    if (msaa) {
        subpass.pResolveAttachments = &resolveRef;
    }

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;

    if (vkCreateRenderPass(device.device(), &info, nullptr, &renderPass_) != VK_SUCCESS) {
        spdlog::error("[backend] vkCreateRenderPass (main) failed");
        return false;
    }
    return true;
}

bool RenderPass::createOffscreen(VulkanDevice& device, VkFormat colorFormat,
                                 VkSampleCountFlagBits samples,
                                 VkImageLayout finalLayout) {
    destroy();
    device_ = &device;
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    VkAttachmentDescription color{};
    color.format = colorFormat;
    color.samples = samples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                         : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : finalLayout;

    VkAttachmentDescription resolve{};
    resolve.format = colorFormat;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolve.finalLayout = finalLayout;

    std::vector<VkAttachmentDescription> attachments = {color};
    if (msaa) {
        attachments.push_back(resolve);
    }

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    if (msaa) {
        subpass.pResolveAttachments = &resolveRef;
    }

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;

    if (vkCreateRenderPass(device.device(), &info, nullptr, &renderPass_) != VK_SUCCESS) {
        spdlog::error("[backend] vkCreateRenderPass (offscreen) failed");
        return false;
    }
    return true;
}

bool RenderPass::createPickPass(VulkanDevice& device, VkFormat idFormat) {
    destroy();
    device_ = &device;

    VkAttachmentDescription id{};
    id.format = idFormat;
    id.samples = VK_SAMPLE_COUNT_1_BIT;
    id.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    id.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    id.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    id.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    id.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    id.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference idRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &idRef;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &id;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;

    if (vkCreateRenderPass(device.device(), &info, nullptr, &renderPass_) != VK_SUCCESS) {
        spdlog::error("[backend] vkCreateRenderPass (pick) failed");
        return false;
    }
    return true;
}

void RenderPass::destroy() {
    if (renderPass_ != VK_NULL_HANDLE && device_) {
        vkDestroyRenderPass(device_->device(), renderPass_, nullptr);
    }
    renderPass_ = VK_NULL_HANDLE;
}

// —— Framebuffer ——

Framebuffer::~Framebuffer() { destroy(); }

bool Framebuffer::create(VulkanDevice& device, VkRenderPass renderPass,
                         const std::vector<VkImageView>& attachments,
                         VkExtent2D extent) {
    destroy();
    device_ = &device;
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    if (vkCreateFramebuffer(device.device(), &info, nullptr, &framebuffer_) != VK_SUCCESS) {
        spdlog::error("[backend] vkCreateFramebuffer failed");
        return false;
    }
    return true;
}

void Framebuffer::destroy() {
    if (framebuffer_ != VK_NULL_HANDLE && device_) {
        vkDestroyFramebuffer(device_->device(), framebuffer_, nullptr);
    }
    framebuffer_ = VK_NULL_HANDLE;
}

} // namespace moleculens::engine
