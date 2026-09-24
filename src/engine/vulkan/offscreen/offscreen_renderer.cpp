// =============================================================================
// MolecuLens · engine · vulkan/offscreen/offscreen_renderer.cpp —— 离屏渲染实现
// 规格来源：主方案 §6.1/§12.5（无 Swapchain；MSAA resolve；像素回读 RGBA8/16F）
// =============================================================================

#include "engine/vulkan/offscreen/offscreen_renderer.h"

#include <algorithm>
#include <cstring>

#include "spdlog/spdlog.h"

#include "engine/render/renderer.h"
#include "engine/vulkan/backend/decode.h"      // 命令流 → DrawList（唯一解码实现）
#include "engine/vulkan/geometry/geometry.h"   // 共享几何/投影（唯一实现）

namespace moleculens::engine {

// —— 生命周期 ——

OffscreenRenderer::~OffscreenRenderer() { shutdown(); }

bool OffscreenRenderer::init(const RenderConfig& cfg, uint32_t width, uint32_t height,
                             std::string* outError) {
    shutdown();
    config_ = cfg;
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);

    ownedDevice_ = std::make_unique<VulkanDevice>();
    device_ = ownedDevice_.get();
    if (!device_->initHeadless(config_, outError)) {
        ownedDevice_.reset();
        device_ = nullptr;
        return false;
    }
    if (!initCommon(outError)) {
        ownedDevice_.reset();
        device_ = nullptr;
        return false;
    }
    return true;
}

// 共享主设备初始化（GUI 视口路径）：FBO/管线建在主设备上，ImGui 描述符集
// 与图像同设备采样；headless 路径（CLI 导出）仍走 init()。
bool OffscreenRenderer::initShared(VulkanDevice& device, uint32_t width,
                                   uint32_t height, std::string* outError) {
    shutdown();
    config_ = device.config();
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    device_ = &device;
    if (!initCommon(outError)) {
        device_ = nullptr;
        return false;
    }
    return true;
}

bool OffscreenRenderer::initCommon(std::string* outError) {
    if (!createFramebuffer()) {
        if (outError) *outError = "offscreen framebuffer failed";
        return false;
    }

    // 全局描述符集（UBO：相机 / 光照 / 雾，主方案 §6.6.4）
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!globalLayout_.create(*device_, {binding}) ||
        !globalUbo_.create(*device_, globalLayout_.layout(), sizeof(GlobalUBO))) {
        return false;
    }

    // 标准管线集（球 / 键 / 线段 / 网格 / 点 × 实心 / 线框 / 透明）统一构建。
    // 离屏 FBO 附件恒为 1x（createFramebuffer），管线采样数必须与之一致：共享主设备
    // （MSAA 4x）时不可用 device.msaaSamples()——采样数不匹配的管线与渲染通道不兼容
    // → 全屏片元不绘制 → 分子空白（见 moleculens-fixes）。
    std::string pipeErr;
    if (!pipelines_.create(*device_, renderPass_.renderPass(), VK_SAMPLE_COUNT_1_BIT,
                           globalLayout_.layout(), &pipeErr)) {
        if (outError) *outError = pipeErr;
        return false;
    }

    meshes_.create(*device_);   // 球/柱单位网格（每个离屏实例一份）
    initialized_ = true;
    spdlog::info("[backend] OffscreenRenderer initialized ({}x{})", width_, height_);
    return true;
}

void OffscreenRenderer::shutdown() {
    if (!initialized_) {
        return;
    }
    vkDeviceWaitIdle(device_->device());
    gpu_.destroy();
    meshes_.destroy();
    pipelines_.destroy(*device_);   // 管线引用渲染通道，须先于 renderPass_.destroy()
    globalUbo_.destroy();
    globalLayout_.destroy();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    destroyFramebuffer();
    renderPass_.destroy();   // 渲染通道在 shutdown 统一销毁（管线已先销毁）
    if (readbackBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_->device(), readbackBuffer_, nullptr);
        vkFreeMemory(device_->device(), readbackMemory_, nullptr);
        readbackBuffer_ = VK_NULL_HANDLE;
        readbackMemory_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
    ownedDevice_.reset();   // 仅 headless 自建设备；共享模式为空（主设备外部持有）
    device_ = nullptr;
}

void OffscreenRenderer::setBackground(const float* rgb, float alpha) {
    clearColor_[0] = rgb[0];
    clearColor_[1] = rgb[1];
    clearColor_[2] = rgb[2];
    clearColor_[3] = alpha;
}

bool OffscreenRenderer::createFramebuffer() {
    destroyFramebuffer();
    // 尺寸上限防护：超过 maxImageDimension2D 时 vkCreateImage 直接失败，
    // 而调用方此前只能看到一句 "framebuffer failed"，无从判断是尺寸过大。
    // 高分辨率导出（4K/8K + SSAA）很容易撞上限，故在此显式检查并报出具体数字。
    {
        VkPhysicalDeviceProperties devProps{};
        vkGetPhysicalDeviceProperties(device_->physicalDevice(), &devProps);
        const uint32_t maxDim = devProps.limits.maxImageDimension2D;
        if (width_ > maxDim || height_ > maxDim) {
            spdlog::error("[backend] 离屏尺寸 {}x{} 超出设备上限 maxImageDimension2D={}",
                          width_, height_, maxDim);
            return false;
        }
    }
    // 无 MSAA（主方案 §12.5：导出 MSAA 可配，M1 先 1x；M4 支持 msaa 参数）
    const VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    if (!colorImage_.create(*device_, colorFormat, width_, height_, samples,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    if (!depthImage_.create(*device_, depthFormat, width_, height_, samples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        return false;
    }
    // 渲染通道不依赖视口尺寸（仅附件格式/采样数/布局），只在首次创建。
    // 若 resize 时重建渲染通道，initCommon 创建的管线（引用旧通道）会失效
    // → draw 无片元 → 离屏图像空白（清屏仍生效，掩盖 draw 失败）。
    if (renderPass_.renderPass() == VK_NULL_HANDLE) {
        if (!renderPass_.createOffscreen(*device_, colorFormat, depthFormat, samples,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            return false;
        }
    }
    if (!framebuffer_.create(*device_, renderPass_.renderPass(),
                             {colorImage_.view(), depthImage_.view()},
                             {width_, height_})) {
        return false;
    }
    // 采样器（线性，ImGui::Image 用）
    if (sampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 1.0f;
        vkCreateSampler(device_->device(), &info, nullptr, &sampler_);
    }
    colorLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void OffscreenRenderer::destroyFramebuffer() {
    framebuffer_.destroy();
    // 渲染通道被管线引用，须保持到 shutdown 才销毁；resize 重建图像/framebuffer
    // 时保留渲染通道（若销毁，管线失效 → 离屏绘制空白）。
    depthImage_.destroy();
    colorImage_.destroy();
}

void OffscreenRenderer::resize(uint32_t width, uint32_t height) {
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    vkDeviceWaitIdle(device_->device());
    destroyFramebuffer();
    createFramebuffer();
}

// —— 渲染 ——

void OffscreenRenderer::execute(const SceneCommandList& list, const RenderView& view) {

    // 队列提交互斥：后台导出线程与主线程视口共用同一 graphics 队列 → 串行提交
    std::unique_lock<std::mutex> qlock = device_->lockQueue();
    // 命令流 → CPU 绘制列表（唯一解码实现：vulkan/backend/decode.cpp）
    decodeCommandList(list, drawList_);

    // UBO
    GlobalUBO ubo{};
    // vkProjection 的 Y 翻转/深度调整须作用于"纯投影矩阵"，再乘视图矩阵。
    // 直接作用于 P*V 联合矩阵只翻 m[1][1]，会把视图的 up 分量翻错 → clip.y 错误、分子不可见。
    ubo.viewProj = vkProjection(view.camera.projectionMatrix(
        static_cast<float>(width_) / static_cast<float>(height_))) *
                   view.camera.viewMatrix();
    ubo.cameraPos = glm::vec4(view.camera.position(), 1.0f);
    // 全局光照从 RenderView 读取（天顶/地面半球色 + 环境光；无方向光/点光源，
    // 见 moleculens-fixes —— 光照可调 + 更新机制经 DIRTY_PROPERTIES 重渲染）
    ubo.lightDir = glm::vec4(view.light.skyColor, 0.0f);
    ubo.lightColor = glm::vec4(view.light.groundColor, 0.0f);
    ubo.ambient = glm::vec4(view.light.ambientColor, view.light.ambientIntensity);
    ubo.fog = view.fog;
    ubo.sceneProps = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    globalUbo_.update(0, ubo);

    // 诊断（几何数量变化时）：确认离屏渲染确实执行且带几何
    {
        static uint32_t sLast = UINT32_MAX;
        const uint32_t total = static_cast<uint32_t>(drawList_.spheres.size() +
                                                     drawList_.cylinders.size() +
                                                     drawList_.lines.size() +
                                                     drawList_.meshes.size());
        if (total != sLast) {
            sLast = total;
            spdlog::info("[backend] offscreen execute: {} spheres, {} cylinders, {} lines, {} meshes @ {}x{}",
                         drawList_.spheres.size(), drawList_.cylinders.size(),
                         drawList_.lines.size(), drawList_.meshes.size(), width_, height_);
        }
    }

    VkCommandBuffer cmd = device_->beginSingleTimeCommands();
    VkViewport viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_),
                        0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width_, height_}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkClearValue clears[] = {
        {clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]},
        {1.0f, 0},
    };
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_.renderPass();
    rpBegin.framebuffer = framebuffer_.framebuffer();
    rpBegin.renderArea = scissor;
    rpBegin.clearValueCount = 2;   // color + depth
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkDescriptorSet globalSet = globalUbo_.set(0);
    // 几何绘制（唯一实现：vulkan/draw/draw_geometry.cpp）
    drawGeometry(cmd, *device_, pipelines_, drawList_, gpu_, meshes_, globalSet);

    vkCmdEndRenderPass(cmd);
    device_->endSingleTimeCommands(cmd);
}

bool OffscreenRenderer::readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt) {
    // 队列提交互斥（与 execute 一致，见 QS.4）
    std::unique_lock<std::mutex> qlock = device_->lockQueue();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ *
                               (fmt == PixelFormat::Rgba16F ? 8 : 4);
    VkCommandBuffer cmd = device_->beginSingleTimeCommands();

    // colorImage → readback buffer
    if (readbackBuffer_ == VK_NULL_HANDLE || readbackSize_ < bytes) {
        if (readbackBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_->device(), readbackBuffer_, nullptr);
            vkFreeMemory(device_->device(), readbackMemory_, nullptr);
        }
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device_->device(), &info, nullptr, &readbackBuffer_);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_->device(), readbackBuffer_, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(device_->physicalDevice(), &props);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                alloc.memoryTypeIndex = i;
                break;
            }
        }
        vkAllocateMemory(device_->device(), &alloc, nullptr, &readbackMemory_);
        vkBindBufferMemory(device_->device(), readbackBuffer_, readbackMemory_, 0);
        readbackSize_ = bytes;
    }

    // 布局转换：SHADER_READ_ONLY → TRANSFER_SRC（拷贝后转回）
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = colorImage_.image();
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(cmd, colorImage_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_, 1, &region);

    // 转回 SHADER_READ_ONLY（供 ImGui 采样）
    VkImageMemoryBarrier back{};
    back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back.image = colorImage_.image();
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &back);
    device_->endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    vkMapMemory(device_->device(), readbackMemory_, 0, bytes, 0, &mapped);
    if (!mapped) {
        return false;
    }
    // GPU 已把图像拷入 readbackBuffer_；若该内存类型非 HOST_COHERENT，须显式
    // invalidate 使主机端缓存失效，否则读到的是陈旧数据（全背景/垃圾）→ 误判渲染空白。
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readbackMemory_;
    range.offset = 0;
    range.size = bytes;
    vkInvalidateMappedMemoryRanges(device_->device(), 1, &range);
    out.assign(static_cast<const uint8_t*>(mapped),
               static_cast<const uint8_t*>(mapped) + bytes);
    pixels_ = out;
    vkUnmapMemory(device_->device(), readbackMemory_);
    return true;
}


} // namespace moleculens::engine
