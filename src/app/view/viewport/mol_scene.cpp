// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.cpp —— 3D 视口实现
// 规格来源：主方案 §7.7（FBO 方案：场景渲染进离屏 FBO，纹理直接给 ImGui 采样）
// =============================================================================

#include "app/view/viewport/mol_scene.h"

#include <imgui_impl_vulkan.h>

#include "engine/camera/camera.h"
#include "engine/export/png/png_encoder.h"
#include "engine/raytrace/ray_tracer.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "entity/scene/scene.h"

namespace moleculens::app {

MolScene::MolScene() = default;

MolScene::~MolScene() { shutdown(); }

const entity::SceneStructure* MolScene::scene() const {
    return renderer_ ? renderer_->scene() : nullptr;
}

void MolScene::init(engine::Renderer* renderer, engine::OrbitCamera* camera,
                    engine::VulkanRenderer* vk) {
    renderer_ = renderer;
    camera_ = camera;
    offscreen_ = std::make_unique<engine::OffscreenRenderer>();
    std::string err;
    // 复用主设备：离屏 FBO 图像创建在主设备上，ImGui 描述符集（主设备 imgui 池）
    // 才能采样到它。若用独立 headless 设备，colorView 属另一 VkDevice，主渲染
    // 通道无法采样 → 视口空白、分子不显示。
    if (vk && offscreen_->initShared(vk->device(), 640, 480, &err)) {
        spdlog::info("[ui] viewport offscreen initialized on main device (640x480)");
        return;
    }
    if (err.empty()) {
        err = "shared-device offscreen init failed";
    }
    spdlog::warn("[ui] viewport offscreen shared-device init failed: {}; falling back to headless", err);
    // 兜底：headless（CLI/无主设备）。注意：headless 设备上创建的图像无法被
    // 主设备 ImGui 采样（跨设备纹理），GUI 视口会空白——仅作最后手段。
    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    if (!offscreen_->init(cfg, 640, 480, &err)) {
        spdlog::error("[ui] offscreen viewport init failed: {}", err);
        offscreen_.reset();
    } else {
        spdlog::warn("[ui] viewport offscreen on headless device (cross-device texture -> 视口可能空白)");
    }
}

void MolScene::shutdown() {
    destroyRtTexture();
    offscreen_.reset();
    texture_ = ImTextureID(0);
}

void MolScene::render(const ImVec2& size) {
    if (!renderer_ || !camera_ || !offscreen_ || !renderer_->scene()) {
        ImGui::Text("3D 视口未初始化（先打开文件）");
        return;
    }
    // 记录逻辑显示尺寸（拖拽平移用：屏幕像素 ↔ NDC 换算须用逻辑尺寸）
    displayW_ = static_cast<uint32_t>(std::max(1.0f, size.x));
    displayH_ = static_cast<uint32_t>(std::max(1.0f, size.y));
    // 每帧兜底重置拖拽状态：无任何鼠标键按下时（release 帧可能无位移，main_view
    // 不转发 → handleMouse 不被调用 → 拖拽状态残留 → 后续手势/换键全部失效）
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) {
        isDragging_ = false;
        dragButton_ = -1;
    }
    // CPU 光线追踪预览：整个视口由 RT 渲染（相机/场景变化时重渲）
    if (rayTracePreview_) {
        renderRayTracePreview(size);
        return;
    }
    // 离屏渲染目标用物理像素分辨率（logical × DisplayFramebufferScale）：
    // 否则高 DPI（缩放 >1）下纹理为逻辑分辨率，放大到物理像素后模糊。
    const ImGuiIO& io = ImGui::GetIO();
    const float fsx = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const float fsy = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    const uint32_t w = static_cast<uint32_t>(std::max(1.0f, size.x * fsx));
    const uint32_t h = static_cast<uint32_t>(std::max(1.0f, size.y * fsy));
    if (w != viewportW_ || h != viewportH_) {
        offscreen_->resize(w, h);
        viewportW_ = w;
        viewportH_ = h;
        dirty_ = true;
        texture_ = ImTextureID(0);   // FBO 重建 → 纹理重建
    }
    // 相机移动 / 场景脏 → 重渲染
    // 除 yaw/pitch/distance/target 外还须检测 fov/ortho（工具栏切换视角）与
    // 场景脏位（背景/表示变更），否则改了视角/背景视图不刷新。
    const bool cameraMoved = (camera_->yaw != lastYaw_ || camera_->pitch != lastPitch_ ||
                              camera_->distance != lastDist_ ||
                              camera_->target != lastTarget_ ||
                              camera_->fov != lastFov_ || camera_->ortho != lastOrtho_ ||
                              camera_->roll != lastRoll_);
    const bool sceneDirty = renderer_->scene()->dirtyBits() != 0;
    if (dirty_ || cameraMoved || sceneDirty ||
        renderer_->commandSerial() != lastSerial_) {
        renderer_->update(0.0f);   // 脏位域 → needsRebuild_（prepare 阶段重建命令流）
        const auto& props = renderer_->scene()->properties();
        offscreen_->setBackground(props.bgColor);
        renderer_->prepare();
        // 诊断（一次性/数量变化时）：数据→命令流几何是否生成，定位断点
        {
            uint32_t nSphere = 0, nCyl = 0, nLine = 0, nMesh = 0;
            const engine::SceneCommandList& cl = renderer_->commandList();
            engine::CommandIterator it(cl);
            for (bool has = cl.first(it); has; has = cl.next(it)) {
                switch (it.code()) {
                    case engine::Cmd::SphereInst:   nSphere += it.payloadSize() / sizeof(engine::SphereInst); break;
                    case engine::Cmd::CylinderInst: nCyl    += it.payloadSize() / sizeof(engine::CylinderInst); break;
                    case engine::Cmd::LineInst:     nLine   += it.payloadSize() / sizeof(engine::LineInst); break;
                    case engine::Cmd::TriMesh:      ++nMesh; break;
                    default: break;
                }
            }
            static uint32_t sLast = UINT32_MAX;
            if (nSphere != sLast) {
                sLast = nSphere;
                spdlog::info("[ui] viewport command list: {} spheres, {} cylinders, {} lines, {} meshes, serial={}",
                             nSphere, nCyl, nLine, nMesh, cl.serial());
            }
        }
        engine::RenderView view;
        view.camera = *camera_;
        view.width = w;
        view.height = h;
        renderer_->applySceneLighting(view);   // 场景光照/雾 → UBO（光照更新机制）
        offscreen_->execute(renderer_->commandList(), view);
        // 一次性诊断：回读离屏图像统计非背景像素——区分"渲染空白"（draw 未写入）
        // 与"图像正常但显示失败"（ImGui 纹理未显示）。
        {
            static bool sReadback = false;
            if (!sReadback) {
                sReadback = true;
                std::vector<uint8_t> px;
                if (offscreen_->readbackPixels(px, engine::PixelFormat::Rgba8)) {
                    uint32_t nonBg = 0;
                    uint32_t centerR = 0, centerG = 0, centerB = 0;
                    for (size_t i = 0; i + 3 < px.size(); i += 4) {
                        if (px[i] > 45 || px[i + 1] > 45 || px[i + 2] > 50) {
                            ++nonBg;
                        }
                    }
                    const size_t c = (static_cast<size_t>(viewportH_) / 2 *
                                          static_cast<size_t>(viewportW_) +
                                      static_cast<size_t>(viewportW_) / 2) *
                                     4;
                    if (c + 2 < px.size()) {
                        centerR = px[c];
                        centerG = px[c + 1];
                        centerB = px[c + 2];
                    }
                    spdlog::info("[ui] viewport offscreen READBACK: {}x{}, non-bg={}, center=({},{},{}), cam target=({},{},{}), dist={}",
                                 viewportW_, viewportH_, nonBg, centerR, centerG, centerB,
                                 camera_->target.x, camera_->target.y, camera_->target.z,
                                 camera_->distance);
                } else {
                    spdlog::error("[ui] viewport offscreen readback FAILED");
                }
            }
        }
        lastSerial_ = renderer_->commandSerial();
        lastYaw_ = camera_->yaw;
        lastPitch_ = camera_->pitch;
        lastDist_ = camera_->distance;
        lastTarget_ = camera_->target;
        lastFov_ = camera_->fov;
        lastOrtho_ = camera_->ortho;
        lastRoll_ = camera_->roll;
        dirty_ = false;
    }
    if (texture_ == ImTextureID(0) && offscreen_->colorView() != VK_NULL_HANDLE) {
        // vcpkg imgui 的 AddTexture 返回 VkDescriptorSet（旧版签名）→ 转 ImTextureID
        const VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
            offscreen_->sampler(), offscreen_->colorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        texture_ = ImTextureID(reinterpret_cast<uintptr_t>(desc));
        spdlog::info("[ui] viewport texture descriptor created (image {}x{}, layout={})",
                     viewportW_, viewportH_,
                     static_cast<int>(offscreen_->colorLayout()));
    }
    if (texture_) {
        ImGui::Image(texture_, size);
    } else {
        ImGui::Text("视口纹理不可用");
    }
}

void MolScene::renderRayTracePreview(const ImVec2& size) {
    if (!renderer_ || !camera_ || !renderer_->scene()) {
        return;
    }
    // 低分辨率预览（宽 ≤ kCapPx，保持宽高比）——CPU RT 按需重渲，交互可跟手
    constexpr uint32_t kCapPx = 420;
    uint32_t rw = std::max(1u, displayW_);
    uint32_t rh = std::max(1u, displayH_);
    if (rw > kCapPx) {
        rh = std::max(1u, static_cast<uint32_t>(static_cast<float>(rh) * kCapPx / rw));
        rw = kCapPx;
    }
    if (rw != rtW_ || rh != rtH_) {
        if (!ensureRtTexture(rw, rh)) {
            ImGui::Text("RT 预览纹理不可用");
            return;
        }
    }
    const bool cameraMoved = (camera_->yaw != lastYaw_ || camera_->pitch != lastPitch_ ||
                              camera_->distance != lastDist_ ||
                              camera_->target != lastTarget_ ||
                              camera_->fov != lastFov_ || camera_->ortho != lastOrtho_ ||
                              camera_->roll != lastRoll_);
    const bool sceneDirty = renderer_->scene()->dirtyBits() != 0;
    if (dirty_ || cameraMoved || sceneDirty ||
        renderer_->commandSerial() != lastSerial_) {
        renderer_->update(0.0f);
        renderer_->prepare();
        engine::RtScene rtScene;
        engine::buildRtScene(renderer_->commandList(), rtScene);
        engine::RtCamera rtc;
        rtc.position = camera_->position();
        rtc.target = camera_->target;
        // 相机 up（含滚转）：视图矩阵第二列（世界空间，与 pan 提取 right 同法）
        const glm::mat4 V = camera_->viewMatrix();
        rtc.up = glm::normalize(glm::vec3(V[0][1], V[1][1], V[2][1]));
        rtc.fovDeg = camera_->fov;
        rtc.width = rw;
        rtc.height = rh;

        engine::RenderView view;
        view.camera = *camera_;
        renderer_->applySceneLighting(view);   // 光照与光栅化一致
        engine::RtLight rtl;
        rtl.skyColor = view.light.skyColor;
        rtl.groundColor = view.light.groundColor;
        rtl.ambientColor = view.light.ambientColor;
        rtl.ambientIntensity = view.light.ambientIntensity;
        rtl.maxBounces = 0;        // 预览：无反射（保证交互帧率）

        const auto& props = renderer_->scene()->properties();
        const glm::vec3 bg(props.bgColor[0], props.bgColor[1], props.bgColor[2]);
        std::vector<uint8_t> rgba;
        engine::rayTrace(rtScene, rtc, rtl, bg, 1, rgba);
        updateRtTexture(rgba);

        lastSerial_ = renderer_->commandSerial();
        lastYaw_ = camera_->yaw;
        lastPitch_ = camera_->pitch;
        lastDist_ = camera_->distance;
        lastTarget_ = camera_->target;
        lastFov_ = camera_->fov;
        lastOrtho_ = camera_->ortho;
        lastRoll_ = camera_->roll;
        dirty_ = false;
    }
    if (rtTexture_) {
        ImGui::Image(rtTexture_, size);
    } else {
        ImGui::Text("RT 预览不可用");
    }
}

bool MolScene::handleMouse(float dx, float dy, int button) {
    if (!camera_) {
        return false;
    }
    // 按钮已抬起：结束本次拖拽。release 帧可能无位移，main_view 不会调用本函数，
    // 因此还需在 render() 每帧做兜底重置（否则拖拽状态残留 → 后续手势失效）。
    if (!ImGui::IsMouseDown(button)) {
        if (dragButton_ == button) {
            isDragging_ = false;
            dragButton_ = -1;
        }
        return false;
    }
    // 首次移动：进入拖拽（点击未移动不触发）
    if (!isDragging_) {
        isDragging_ = true;
        dragButton_ = button;
    }
    if (dragButton_ != button) {
        return false;   // 其它按钮的拖拽：忽略（避免多键串扰）
    }
    // 主方案 §7.10：左键轨道旋转 / 中键缩放 / 右键绕屏幕法线滚转（Shift+右键平移）
    const float sensitivity = 0.008f;
    if (button == 0) {
        // 左键轨道：视角跟随鼠标拖拽方向同步转动。
        //  水平 +dx：拖右 → yaw 增 → 相机绕到 +X（场景右侧），视角转向右侧；
        //  竖直 -dy：拖上（dy<0）→ pitch 增 → 相机升高，看到顶面。
        camera_->orbit(dx * sensitivity, -dy * sensitivity);
    } else if (button == 1) {
        camera_->dolly(1.0f + dy * sensitivity);
    } else if (button == 2) {
        const bool shift = ImGui::GetIO().KeyShift;
        if (!shift) {
            // 右键：绕屏幕法线（指向屏幕外）滚转，顺时针/逆时针随拖拽方向
            // （正 roll = 逆时针，见 OrbitCamera::rollView 注释；拖右→顺时针）
            camera_->rollView(-dx * 0.004f);
        } else {
            // Shift+右键：屏幕空间平移（保留平移能力）
            const float w = static_cast<float>(std::max(1u, displayW_));
            const float h = static_cast<float>(std::max(1u, displayH_));
            camera_->pan(glm::vec2(dx / w * 2.0f, -dy / h * 2.0f));
        }
    }
    return true;
}

void MolScene::handleScroll(float scrollY) {
    if (camera_) {
        // 滚轮缩放（主方案 §7.10：1 - scroll*0.15）
        camera_->dolly(1.0f - scrollY * 0.15f);
    }
}

void MolScene::handleClick() {
    // M2：拾取（ID Pass / CPU 射线）
}

bool MolScene::exportToPng(uint32_t width, uint32_t height,
                           const std::string& path, const float bgOverride[3],
                           int ssaa) {
    if (!renderer_ || !camera_ || !offscreen_ || !renderer_->scene()) {
        spdlog::error("[io] export: 渲染器/相机/场景未就绪");
        return false;
    }
    if (width == 0 || height == 0) {
        spdlog::error("[io] export: 非法尺寸 {}x{}", width, height);
        return false;
    }
    const int ss = std::max(1, ssaa);
    const uint32_t rw = width * static_cast<uint32_t>(ss);
    const uint32_t rh = height * static_cast<uint32_t>(ss);
    // 确保命令流最新（脏则重建）
    renderer_->update(0.0f);
    renderer_->prepare();
    // 独立离屏渲染器（共享主设备；避免改动视口自身 FBO）
    engine::OffscreenRenderer exp;
    std::string err;
    if (!exp.initShared(offscreen_->device(), rw, rh, &err)) {
        spdlog::error("[io] export: offscreen init failed: {}", err);
        return false;
    }
    const auto& props = renderer_->scene()->properties();
    if (bgOverride) {
        exp.setBackground(bgOverride);
    } else {
        exp.setBackground(props.bgColor);
    }
    engine::RenderView view;
    view.camera = *camera_;
    view.width = rw;
    view.height = rh;
    renderer_->applySceneLighting(view);
    exp.execute(renderer_->commandList(), view);
    std::vector<uint8_t> px;
    if (!exp.readbackPixels(px, engine::PixelFormat::Rgba8)) {
        spdlog::error("[io] export: 像素回读失败");
        return false;
    }
    // 超采样（SSAA）：把 rw×rh 盒式降采样回 width×height → 抗锯齿
    if (ss > 1) {
        std::vector<uint8_t> outPx(static_cast<size_t>(width) * height * 4);
        const uint32_t n = static_cast<uint32_t>(ss) * ss;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t r = 0, g = 0, b = 0, a = 0;
                for (int dy = 0; dy < ss; ++dy) {
                    for (int dx = 0; dx < ss; ++dx) {
                        const size_t s =
                            (static_cast<size_t>(y * ss + dy) * rw +
                             static_cast<size_t>(x * ss + dx)) * 4;
                        r += px[s];
                        g += px[s + 1];
                        b += px[s + 2];
                        a += px[s + 3];
                    }
                }
                const size_t d = (static_cast<size_t>(y) * width + x) * 4;
                outPx[d] = static_cast<uint8_t>(r / n);
                outPx[d + 1] = static_cast<uint8_t>(g / n);
                outPx[d + 2] = static_cast<uint8_t>(b / n);
                outPx[d + 3] = static_cast<uint8_t>(a / n);
            }
        }
        px.swap(outPx);
    }
    if (!engine::writePng(path, width, height, px)) {
        spdlog::error("[io] export: PNG 编码/写入失败");
        return false;
    }
    return true;
}

bool MolScene::exportRayTracedToPng(uint32_t width, uint32_t height,
                                    const std::string& path,
                                    const float bgOverride[3], int samplesPerPixel) {
    if (!renderer_ || !camera_ || !renderer_->scene()) {
        spdlog::error("[io] export(rt): 渲染器/相机/场景未就绪");
        return false;
    }
    renderer_->update(0.0f);
    renderer_->prepare();
    engine::RtScene rtScene;
    engine::buildRtScene(renderer_->commandList(), rtScene);
    engine::RtCamera rtc;
    rtc.position = camera_->position();
    rtc.target = camera_->target;
    const glm::mat4 V = camera_->viewMatrix();
    rtc.up = glm::normalize(glm::vec3(V[0][1], V[1][1], V[2][1]));
    rtc.fovDeg = camera_->fov;
    rtc.width = width;
    rtc.height = height;

    engine::RenderView view;
    view.camera = *camera_;
    renderer_->applySceneLighting(view);
    engine::RtLight rtl;
    rtl.skyColor = view.light.skyColor;
    rtl.groundColor = view.light.groundColor;
    rtl.ambientColor = view.light.ambientColor;
    rtl.ambientIntensity = view.light.ambientIntensity;
    rtl.maxBounces = 1;       // 导出：球体 1 次反射

    const auto& props = renderer_->scene()->properties();
    glm::vec3 bg(props.bgColor[0], props.bgColor[1], props.bgColor[2]);
    if (bgOverride) {
        bg = glm::vec3(bgOverride[0], bgOverride[1], bgOverride[2]);
    }
    std::vector<uint8_t> rgba;
    engine::rayTrace(rtScene, rtc, rtl, bg, std::max(1, samplesPerPixel), rgba);
    if (!engine::writePng(path, width, height, rgba)) {
        spdlog::error("[io] export(rt): PNG 编码/写入失败");
        return false;
    }
    spdlog::info("[io] export(rt): {} -> {} ({}x{})", path, width, height);
    return true;
}

bool MolScene::ensureRtTexture(uint32_t w, uint32_t h) {
    if (rtImage_ != VK_NULL_HANDLE && rtW_ == w && rtH_ == h) {
        return true;
    }
    destroyRtTexture();
    if (!offscreen_) {
        return false;
    }
    engine::VulkanDevice& dev = offscreen_->device();
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_LINEAR;   // 线性 tiling：可直接 host 写
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev.device(), &ci, nullptr, &rtImage_) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev.device(), rtImage_, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(dev.physicalDevice(), &props);
    int32_t typeIdx = -1;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            typeIdx = static_cast<int32_t>(i);
            rtCoherent_ = true;
            break;
        }
    }
    if (typeIdx < 0) {
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                typeIdx = static_cast<int32_t>(i);
                rtCoherent_ = false;
                break;
            }
        }
    }
    if (typeIdx < 0) {
        destroyRtTexture();
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(typeIdx);
    if (vkAllocateMemory(dev.device(), &ai, nullptr, &rtImageMem_) != VK_SUCCESS) {
        destroyRtTexture();
        return false;
    }
    vkBindImageMemory(dev.device(), rtImage_, rtImageMem_, 0);

    VkImageSubresource sub{};
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(dev.device(), rtImage_, &sub, &layout);
    rtRowPitch_ = layout.rowPitch;

    // 布局 UNDEFINED → GENERAL（host 写 + GPU 采样）；队列提交互斥（QS.4）
    std::unique_lock<std::mutex> qlock = dev.lockQueue();
    VkCommandBuffer cmd = dev.beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = rtImage_;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = 0;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
    dev.endSingleTimeCommands(cmd);

    vkMapMemory(dev.device(), rtImageMem_, 0, VK_WHOLE_SIZE, 0, &rtMapped_);
    rtW_ = w;
    rtH_ = h;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = rtImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device(), &vci, nullptr, &rtView_) != VK_SUCCESS) {
        destroyRtTexture();
        return false;
    }
    const VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
        offscreen_->sampler(), rtView_, VK_IMAGE_LAYOUT_GENERAL);
    rtTexture_ = ImTextureID(reinterpret_cast<uintptr_t>(desc));
    return true;
}

void MolScene::updateRtTexture(const std::vector<uint8_t>& rgba) {
    if (!rtMapped_ || !rtImage_ || rgba.size() < static_cast<size_t>(rtW_) * rtH_ * 4) {
        return;
    }
    const size_t srcRow = static_cast<size_t>(rtW_) * 4u;
    uint8_t* dst = static_cast<uint8_t*>(rtMapped_);
    const uint8_t* src = rgba.data();
    for (uint32_t y = 0; y < rtH_; ++y) {
        std::memcpy(dst + y * rtRowPitch_, src + y * srcRow, srcRow);
    }
    // 非 HOST_COHERENT 内存须显式 flush（使写入对 GPU 可见）
    if (!rtCoherent_) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = rtImageMem_;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(offscreen_->device().device(), 1, &range);
    }
}

void MolScene::destroyRtTexture() {
    if (offscreen_) {
        engine::VulkanDevice& dev = offscreen_->device();
        if (rtMapped_) {
            vkUnmapMemory(dev.device(), rtImageMem_);
            rtMapped_ = nullptr;
        }
        if (rtView_ != VK_NULL_HANDLE) {
            vkDestroyImageView(dev.device(), rtView_, nullptr);
            rtView_ = VK_NULL_HANDLE;
        }
        if (rtImage_ != VK_NULL_HANDLE) {
            vkDestroyImage(dev.device(), rtImage_, nullptr);
            rtImage_ = VK_NULL_HANDLE;
        }
        if (rtImageMem_ != VK_NULL_HANDLE) {
            vkFreeMemory(dev.device(), rtImageMem_, nullptr);
            rtImageMem_ = VK_NULL_HANDLE;
        }
    }
    rtTexture_ = ImTextureID(0);
    rtW_ = 0;
    rtH_ = 0;
}

ImTextureID MolScene::textureId() const {
    return texture_;
}

} // namespace moleculens::app
