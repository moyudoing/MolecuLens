// =============================================================================
// MolecuLens · engine · vulkan/offscreen/offscreen_renderer.cpp —— 离屏渲染实现
// 规格来源：主方案 §6.1/§12.5（无 Swapchain；MSAA resolve；像素回读 RGBA8/16F）
// =============================================================================

#include "engine/vulkan/offscreen/offscreen_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "spdlog/spdlog.h"

#include "engine/render/renderer.h"
#include "engine/vulkan/backend/decode.h"      // 命令流 → DrawList（唯一解码实现）
#include "engine/vulkan/geometry/geometry.h"   // 共享几何/投影（唯一实现）

namespace moleculens::engine {

namespace {
// 阴影贴图边长：与实时后端共用 engine::kDefaultShadowMapSize（离屏实例各持一张）
constexpr uint32_t kShadowMapSize =
    static_cast<uint32_t>(kDefaultShadowMapSize);
} // namespace

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

    // 全局描述符集：binding 0 = UBO（相机/光照/雾/光矩阵），binding 1 = 阴影深度图。
    // 顺序：布局 → 阴影贴图（管线要用布局）→ UBO 描述符集（要写阴影 view/sampler）。
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!globalLayout_.create(*device_, {bindings[0], bindings[1]})) {
        if (outError) *outError = "global descriptor layout failed";
        return false;
    }
    std::string shadowErr;
    if (!shadow_.create(*device_, kShadowMapSize, globalLayout_.layout(), &shadowErr)) {
        spdlog::warn("[backend] shadow map unavailable: {}", shadowErr);
    }
    if (!globalUbo_.create(*device_, globalLayout_.layout(), sizeof(GlobalUBO),
                           shadow_.view(), shadow_.sampler())) {
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

    meshes_.create(*device_, meshDetail_, meshSegments_);   // 球/柱单位网格（离屏一份，档位可调）
    meshesLow_.create(*device_, 2, 12);                      // 播放档：320 面 / 12 段
    initialized_ = true;
    spdlog::info("[backend] OffscreenRenderer initialized ({}x{})", width_, height_);
    return true;
}

void OffscreenRenderer::shutdown() {
    if (!initialized_) {
        return;
    }
    vkDeviceWaitIdle(device_->device());
    shadow_.destroy();
    gpu_.destroy();
    meshes_.destroy();
    meshesLow_.destroy();
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
    // 解码缓存失效（下次 execute 必重解码；gpu_ 已销毁 → 上传修订号亦复位）
    decodedList_ = nullptr;
    decodedRevision_ = UINT64_MAX;
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
    // 颜色附件用 **_SRGB**：主交换链是 B8G8R8A8_SRGB（硬件写时做 linear→sRGB 编码），
    // 而离屏此前是 R8G8B8A8_**UNORM**（不编码）→ 着色器输出的线性值被原样写进 PNG，
    // **导出图比视口暗一档**（同一场景两处 gamma 不一致）。改用 SRGB 附件后，
    // 硬件编码与主交换链一致 → 视口/离屏导出/CPU 光追（ray_tracer 自带 pow(1/2.2)）
    // 三者观感统一（主方案 §6.6 统一 gamma 处理）。
    const VkFormat colorFormat = colorFormat_;
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

void OffscreenRenderer::setMeshQuality(int detail, int segments) {
    const int d = detail < 1 ? 1 : (detail > 4 ? 4 : detail);
    const int s = segments < 3 ? 3 : segments;
    if (d == meshDetail_ && s == meshSegments_) {
        return;
    }
    meshDetail_ = d;
    meshSegments_ = s;
    if (!initialized_ || device_ == nullptr) {
        return;   // 尚未初始化：initCommon 会按新档位创建
    }
    vkDeviceWaitIdle(device_->device());
    meshes_.destroy();
    meshes_.create(*device_, meshDetail_, meshSegments_);
}

bool OffscreenRenderer::setColorAttachment(ColorAttachment fmt) {
    if (fmt == attachment_) {
        return true;
    }
    if (!initialized_ || device_ == nullptr) {
        return false;
    }
    const VkFormat want = (fmt == ColorAttachment::Rgba16Float)
                              ? VK_FORMAT_R16G16B16A16_SFLOAT
                              : VK_FORMAT_R8G8B8A8_SRGB;

    // 设备能力检查：既要能当颜色附件，也要支持混合（透明管线需要）。
    // 缺失时**保持原格式**并返回 false —— 绝不半途改成不可用的格式。
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), want, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    if ((props.optimalTilingFeatures & need) != need) {
        spdlog::error("[backend] 颜色附件格式 {} 不受支持（optimalTilingFeatures=0x{:x}，"
                      "需 COLOR_ATTACHMENT|COLOR_ATTACHMENT_BLEND）",
                      fmt == ColorAttachment::Rgba16Float
                          ? "R16G16B16A16_SFLOAT"
                          : "R8G8B8A8_SRGB",
                      props.optimalTilingFeatures);
        return false;
    }

    vkDeviceWaitIdle(device_->device());
    // 顺序要紧：管线引用渲染通道，必须先销毁管线，再销毁渲染通道；随后按新格式
    // 重建渲染通道（createFramebuffer 在 renderPass_ 为空时创建），最后重建管线。
    pipelines_.destroy(*device_);
    renderPass_.destroy();
    destroyFramebuffer();
    attachment_ = fmt;
    colorFormat_ = want;
    // 附件格式变了 → 上一份回读数据与内容修订都失效
    readbackSize_ = 0;
    pixels_.clear();

    if (!createFramebuffer()) {
        spdlog::error("[backend] 切换颜色附件格式失败（FBO 重建）");
        return false;
    }
    std::string err;
    if (!pipelines_.create(*device_, renderPass_.renderPass(), VK_SAMPLE_COUNT_1_BIT,
                           globalLayout_.layout(), &err)) {
        spdlog::error("[backend] 切换颜色附件格式失败（管线重建）: {}", err);
        return false;
    }
    spdlog::info("[backend] 离屏颜色附件: {}", fmt == ColorAttachment::Rgba16Float
                                                   ? "R16G16B16A16_SFLOAT"
                                                   : "R8G8B8A8_SRGB");
    return true;
}

// —— 渲染 ——

void OffscreenRenderer::execute(const SceneCommandList& list, const RenderView& view) {

    // 队列提交互斥：后台导出线程与主线程视口共用同一 graphics 队列 → 串行提交
    std::unique_lock<std::mutex> qlock = device_->lockQueue();
    // 命令流 → CPU 绘制列表（唯一解码实现：vulkan/backend/decode.cpp）。
    // 缓存键 = (同一命令流对象, serial)：serial 未变即内容未变，跳过整条重解码。
    const uint64_t revision = list.serial();
    if (decodedList_ != &list || decodedRevision_ != revision) {
        decodeCommandList(list, drawList_);
        decodedList_ = &list;
        decodedRevision_ = revision;
    }

    // UBO
    GlobalUBO ubo{};
    // vkProjection 的 Y 翻转/深度调整须作用于"纯投影矩阵"，再乘视图矩阵。
    // 直接作用于 P*V 联合矩阵只翻 m[1][1]，会把视图的 up 分量翻错 → clip.y 错误、分子不可见。
    ubo.viewProj = vkProjection(view.camera.projectionMatrix(
        static_cast<float>(width_) / static_cast<float>(height_))) *
                   view.camera.viewMatrix();
    ubo.cameraPos = glm::vec4(view.camera.position(), 1.0f);
    // 光照（半球环境光 + 方向光）：与实时后端共用同一填装实现，避免布局漂移
    fillGlobalLighting(ubo, view.light);
    // 背景以**实际清屏色**为准（CLI --bg white/transparent、GUI 取色器都走
    // setBackground），而不是场景属性里的默认色 —— 否则雾会淡向另一个颜色。
    fillRenderStyle(ubo, view.style,
                    glm::vec3(clearColor_[0], clearColor_[1], clearColor_[2]));
    // 剖切面（仅进 UBO；不重建命令流/管线）
    fillClipPlane(ubo, view.clipPlane, view.clipEnabled);
    ubo.fog = view.fog;
    // x = 全局缩放, y = 视口高度(px), z = 2·tan(fov/2)（点模式按距离算点大小）
    ubo.sceneProps = glm::vec4(std::max(view.scale, 1e-3f),
                               static_cast<float>(height_),
                               2.0f * std::tan(glm::radians(view.camera.fov) * 0.5f),
                               0.0f);
    globalUbo_.update(0, ubo);

    // 动态帧（播放/拖时间轴）：低细节网格 + 跳过阴影 pass。阴影 pass 会让每帧
    // 的顶点工作量翻倍，而播放时用户关注的是"动起来顺不顺"，不是阴影精度。
    const bool lowDetail = view.lowDetail && meshesLow_.icosphereIndices > 0;
    const SharedMeshes& activeMeshes = lowDetail ? meshesLow_ : meshes_;

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

    // —— 方向光阴影：光源视角的深度 pass（在主渲染通道**之外**，同一命令缓冲）——
    // 光源 UBO 只把 viewProj 换成光矩阵，其余字段与主 UBO 一致（见实时后端注释）。
    if (shadow_.valid() && !lowDetail) {
        GlobalUBO lightUbo = ubo;
        lightUbo.viewProj = view.light.lightViewProj;
        shadow_.record(cmd, *device_, drawList_, meshes_, lightUbo, /*frameIndex=*/0,
                       revision);
    }

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
    drawGeometry(cmd, *device_, pipelines_, drawList_, gpu_, activeMeshes, globalSet, revision,
                 list.meshSerial(), &view);

    vkCmdEndRenderPass(cmd);
    device_->endSingleTimeCommands(cmd);
}

bool OffscreenRenderer::readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt) {
    // 回读格式必须与颜色附件**严格一致**：不一致时会按错误的每像素字节数从图像
    // 里拷贝（例如按 8 B/px 读一个 8-bit 图像）→ 结果"看似成功、内容不可用"。
    // 宁可显式失败，也不返回能解码但没意义的像素。
    const bool want16 = (fmt == PixelFormat::Rgba16F);
    const bool is16 = (colorFormat_ == VK_FORMAT_R16G16B16A16_SFLOAT);
    if (want16 != is16) {
        spdlog::error("[backend] readbackPixels 格式不匹配：请求 {}，附件为 {}"
                      "（先用 setColorAttachment 切换附件）",
                      want16 ? "Rgba16F" : "Rgba8",
                      is16 ? "Rgba16Float(R16G16B16A16_SFLOAT)"
                           : "Rgba8Srgb(R8G8B8A8_SRGB)");
        return false;
    }
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
