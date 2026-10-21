// =============================================================================
// MolecuLens · engine · vulkan/realtime/vulkan_renderer.cpp —— Vulkan 渲染器实现
// 规格来源：主方案 §6.1/§6.3/§6.4（初始化流程；帧循环；命令流 → 缓冲编译；
//           共享网格 icosphere/圆柱；材质 push constant）
// =============================================================================

#include "engine/vulkan/realtime/vulkan_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <GLFW/glfw3.h>

#include "spdlog/spdlog.h"

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/backend/decode.h"      // 命令流 → DrawList（唯一解码实现）
#include "engine/vulkan/geometry/geometry.h"   // 共享几何/投影（唯一实现）

namespace {
// 阴影贴图边长由 engine::kDefaultShadowMapSize 统一提供（render_parameter.h），
// 与 LightParams::shadowMapSize 保持一致；此处不再单独硬编码。
constexpr uint32_t kShadowMapSize =
    static_cast<uint32_t>(moleculens::engine::kDefaultShadowMapSize);
} // namespace

namespace moleculens::engine {

VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::init(WindowHandle window, const RenderConfig& cfg,
                          std::string* outError) {
    shutdown();
    config_ = cfg;
    if (!device_.init(window, cfg, outError)) {
        return false;
    }
    if (!device_.createSwapchain()) {
        if (outError) *outError = "createSwapchain failed";
        return false;
    }
    // 主渲染通道
    if (!mainRenderPass_.createMain(device_, device_.swapchainFormat(),
                                    device_.depthFormat(), device_.msaaSamples())) {
        return false;
    }
    // 全局描述符：binding 0 = UBO（相机/光照/雾/光矩阵），binding 1 = 阴影深度图。
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
    if (!globalLayout_.create(device_, {bindings[0], bindings[1]})) {
        return false;
    }
    // 阴影贴图（深度-only pass + set 0 binding 1 的采样源）
    std::string shadowErr;
    if (!shadow_.create(device_, kShadowMapSize, globalLayout_.layout(), &shadowErr)) {
        spdlog::warn("[backend] shadow map unavailable: {}", shadowErr);
    }
    if (!globalUbo_.create(device_, globalLayout_.layout(), sizeof(GlobalUBO),
                           shadow_.view(), shadow_.sampler())) {
        return false;
    }
    if (!createPipelines()) {
        return false;
    }
    meshes_.create(device_);      // 球/柱单位网格（常驻）
    createImGuiPool();
    if (!createMainFramebuffers()) {
        return false;
    }
    initialized_ = true;
    spdlog::info("[backend] VulkanRenderer initialized ({}x{}, msaa={})",
                 device_.swapchainExtent().width, device_.swapchainExtent().height,
                 static_cast<uint32_t>(device_.msaaSamples()));
    return true;
}

void VulkanRenderer::shutdown() {
    if (!initialized_) {
        return;
    }
    vkDeviceWaitIdle(device_.device());
    destroyPickResources();
    shadow_.destroy();
    gpu_.destroy();
    meshes_.destroy();
    pipelines_.destroy(device_);
    globalUbo_.destroy();
    globalLayout_.destroy();
    destroyMainFramebuffers();
    mainRenderPass_.destroy();
    // imguiPool_ 所有权归 ImGui_ImplVulkan 后端（其 Shutdown 会销毁传入的 pool，
    // 见 imgui_impl_vulkan.cpp Shutdown）；此处不得重复销毁（双重释放会崩溃）。
    imguiPool_ = VK_NULL_HANDLE;
    device_.destroySwapchain();
    initialized_ = false;
    // 解码缓存失效（gpu_ 已销毁 → 上传修订号亦由 FrameGpuBuffers::destroy 复位）
    decodedList_ = nullptr;
    decodedRevision_ = UINT64_MAX;
}

// —— 主渲染附件与帧缓冲 ——
bool VulkanRenderer::createMainFramebuffers() {
    destroyMainFramebuffers();
    const VkExtent2D extent = device_.swapchainExtent();
    const VkFormat colorFormat = device_.swapchainFormat();
    const VkSampleCountFlagBits samples = device_.msaaSamples();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    if (msaa) {
        if (!msaaImage_.create(device_, colorFormat, extent.width, extent.height,
                               samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            return false;
        }
    }
    if (!depthImage_.create(device_, device_.depthFormat(), extent.width,
                            extent.height, samples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        return false;
    }
    swapchainFramebuffers_.resize(device_.swapchainViews().size());
    for (size_t i = 0; i < device_.swapchainViews().size(); ++i) {
        std::vector<VkImageView> attachments;
        if (msaa) {
            attachments.push_back(msaaImage_.view());
            attachments.push_back(depthImage_.view());
            attachments.push_back(device_.swapchainViews()[i]);  // resolve
        } else {
            attachments.push_back(device_.swapchainViews()[i]);
            attachments.push_back(depthImage_.view());
        }
        if (!swapchainFramebuffers_[i].create(device_, mainRenderPass_.renderPass(),
                                              attachments, extent)) {
            return false;
        }
    }
    spdlog::info("[backend] main framebuffers created ({}x{}, msaa={})",
                 extent.width, extent.height, static_cast<uint32_t>(samples));
    return true;
}

void VulkanRenderer::destroyMainFramebuffers() {
    for (auto& fb : swapchainFramebuffers_) {
        fb.destroy();
    }
    swapchainFramebuffers_.clear();
    depthImage_.destroy();
    msaaImage_.destroy();
}

bool VulkanRenderer::createPipelines() {
    // 标准管线集（球/键/线段/网格/点 × 实心/线框/透明）统一构建与销毁
    std::string err;
    if (!pipelines_.create(device_, mainRenderPass_.renderPass(), device_.msaaSamples(),
                           globalLayout_.layout(), &err)) {
        spdlog::error("[backend] pipeline set create failed: {}", err);
        return false;
    }
    return true;
}

void VulkanRenderer::createImGuiPool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 100},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100},
    };
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = 1000;
    info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_.device(), &info, nullptr, &imguiPool_);
}

// —— 帧循环 ——

void VulkanRenderer::beginFrame() {
    // 阶段跟踪（每 60 帧）：B=beginFrame, R=record, E=endFrame
    static uint32_t s_tick = 0;
    if ((s_tick++ % 60) == 0) {
    }
    // 等待 fence（该帧图像可用）
    vkWaitForFences(device_.device(), 1, &device_.inFlightFences()[frameIndex_],
                    VK_TRUE, UINT64_MAX);
    const uint32_t result = device_.acquireNextImage(
        device_.imageAvailable()[frameIndex_], &imageIndex_);
    if (result == 1) {
        // 交换链过期 → 重建帧缓冲并跳过本帧
        device_.recreateSwapchain();
        createMainFramebuffers();
        resized_ = false;
        return;
    }
    if (result == 2) {
        return;
    }
    vkResetFences(device_.device(), 1, &device_.inFlightFences()[frameIndex_]);
    // 上一帧同一槽位的命令缓冲此刻已由 fence 确认 GPU 使用完毕，可以安全释放
    //（endFrame 只结束记录，释放推迟到这里，避免提交已释放句柄）。
    if (frameCommandSlots_[frameIndex_] != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_.device(), device_.commandPool(), 1,
                             &frameCommandSlots_[frameIndex_]);
        frameCommandSlots_[frameIndex_] = VK_NULL_HANDLE;
    }
    frameCommand_ = device_.beginFrameCommands();
    frameCommandSlots_[frameIndex_] = frameCommand_;
}

void VulkanRenderer::execute(const SceneCommandList& list, const RenderView& view) {
    if (frameCommand_ == VK_NULL_HANDLE) {
        return;
    }
    recordFrame(list, view);
}

void VulkanRenderer::endFrame() {
    if (frameCommand_ == VK_NULL_HANDLE) {
        return;
    }
    // 队列提交互斥：与后台导出线程串行（QS.4 图像导出多线程）
    std::unique_lock<std::mutex> qlock = device_.lockQueue();
    device_.endFrameCommands(frameCommand_);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &device_.imageAvailable()[frameIndex_];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frameCommand_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &device_.renderFinished()[frameIndex_];

    const VkResult subResult = vkQueueSubmit(device_.graphicsQueue(), 1, &submit,
                      device_.inFlightFences()[frameIndex_]);
    if (subResult != VK_SUCCESS) {
        spdlog::error("[backend] vkQueueSubmit failed");
    }
    const VkResult presentResult =
        device_.presentImage(imageIndex_, device_.renderFinished()[frameIndex_]);
    (void)presentResult;   // M2：present 结果处理（OUT_OF_DATE → 重建交换链）

    frameIndex_ = (frameIndex_ + 1) % device_.maxFramesInFlight();
    frameCommand_ = VK_NULL_HANDLE;
}

void VulkanRenderer::resize(uint32_t w, uint32_t h) {
    (void)w; (void)h;
    resized_ = true;
}

// —— 命令流执行（主方案 §6.4）——

void VulkanRenderer::recordFrame(const SceneCommandList& list, const RenderView& view) {
    // 命令流 → CPU 绘制列表（唯一解码实现：vulkan/backend/decode.cpp）。
    // 缓存键 = (同一命令流对象, serial)：serial 未变即内容未变，跳过整条重解码。
    const uint64_t revision = list.serial();
    if (decodedList_ != &list || decodedRevision_ != revision) {
        decodeCommandList(list, drawList_);
        decodedList_ = &list;
        decodedRevision_ = revision;
    }

    // UBO 更新（相机 + 光照 + 场景属性，主方案 §6.6.4）
    GlobalUBO ubo{};
    // vkProjection 作用于纯投影矩阵（Y 翻转/深度调整），再乘视图矩阵；
    // 直接作用于 P*V 联合矩阵会只翻 m[1][1] → clip.y 错误、几何不可见。
    ubo.viewProj = vkProjection(view.camera.projectionMatrix(
        device_.swapchainExtent().width / static_cast<float>(
            std::max(1u, device_.swapchainExtent().height)))) *
                   view.camera.viewMatrix();
    ubo.cameraPos = glm::vec4(view.camera.position(), 1.0f);
    // 光照（半球环境光 + 方向光）：与离屏后端共用同一填装实现，避免布局漂移
    fillGlobalLighting(ubo, view.light);
    // 风格 + 背景：雾/距离淡出必须淡到与清屏相同的颜色
    fillRenderStyle(ubo, view.style, view.backgroundColor);
    // 剖切面（仅进 UBO；不重建命令流/管线）
    fillClipPlane(ubo, view.clipPlane, view.clipEnabled);
    ubo.fog = view.fog;
    // 全局缩放（sceneProps.x）此前恒 1.0、SceneProperties::scale 无人消费 →
    // 由 RenderView 携带（渲染设置面板"全局缩放"从此真实生效）
    // x = 全局缩放, y = 视口高度(px), z = 2·tan(fov/2)：点渲染模式按透视距离
    // 计算 gl_PointSize（此前固定 24×半径，远近一样大，点模式观感完全错误）
    ubo.sceneProps = glm::vec4(
        std::max(view.scale, 1e-3f),
        static_cast<float>(device_.swapchainExtent().height),
        2.0f * std::tan(glm::radians(view.camera.fov) * 0.5f), 0.0f);
    globalUbo_.update(frameIndex_, ubo);

    // —— 方向光阴影：先跑光源视角的深度 pass（必须在主渲染通道**之外**）——
    // 光源 UBO 与主 UBO 只差 viewProj（换成光矩阵）；其余字段必须一致，
    // 尤其 sceneProps.x：顶点着色器用它缩放世界坐标，不一致会让深度几何与
    // 主几何错位 → 阴影整体偏移。
    if (shadow_.valid()) {
        GlobalUBO lightUbo = ubo;
        lightUbo.viewProj = view.light.lightViewProj;
        shadow_.record(frameCommand_, device_, drawList_, meshes_, lightUbo, frameIndex_,
                       revision);
    }

    // —— 记录渲染 ——
    VkCommandBuffer cmd = frameCommand_;
    const VkExtent2D extent = device_.swapchainExtent();
    VkViewport viewport{0, 0, static_cast<float>(extent.width),
                        static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkClearValue clearColor = {0.05f, 0.05f, 0.08f, 1.0f};
    const VkClearValue clearDepth = {1.0f, 0};
    const VkClearValue clears[] = {clearColor, clearDepth};
    if (imageIndex_ >= swapchainFramebuffers_.size()) {
        return;
    }
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = mainRenderPass_.renderPass();
    rpBegin.framebuffer = swapchainFramebuffers_[imageIndex_].framebuffer();
    rpBegin.renderArea = scissor;
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkDescriptorSet globalSet = globalUbo_.set(frameIndex_);
    // 几何绘制（唯一实现：vulkan/draw/draw_geometry.cpp）
    drawGeometry(cmd, device_, pipelines_, drawList_, gpu_, meshes_, globalSet, revision, &view);

    // ImGui（UI 回调；renderPass 内）
    if (uiRenderFn_) {
        uiRenderFn_(cmd);
    }

    vkCmdEndRenderPass(cmd);
}

// —— 拾取资源（M2 骨架：离屏 R32_UINT FBO + pick 管线待落地） ——

bool VulkanRenderer::createPickResources() {
    if (pickResources_) {
        return true;
    }
    pickResources_ = std::make_unique<PickResources>();
    spdlog::info("[backend] pick resources created");
    return true;
}

void VulkanRenderer::destroyPickResources() {
    pickResources_.reset();
}


} // namespace moleculens::engine
