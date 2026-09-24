// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.cpp —— 3D 视口实现
// 规格来源：主方案 §7.7（FBO 方案：场景渲染进离屏 FBO，纹理直接给 ImGui 采样）
// =============================================================================

#include "app/view/viewport/mol_scene.h"

#include <imgui_impl_vulkan.h>

#include "app/controller/camera/camera_controller.h"
#include "engine/camera/camera.h"
#include "engine/export/png/png_encoder.h"
#include "engine/raytrace/ray_tracer.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/backend/decode.h"   // CommandStats（MOLECULENS_RENDER_DIAG）
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "entity/scene/scene.h"

// 渲染诊断开关（默认关）。置 1 时在视口打印命令流几何统计与一次性离屏像素回读——
// 用于定位"数据 → 命令流 → GPU"链路断点（见 moleculens-fixes 的排查方法）。
// 默认关闭：诊断会做一次 GPU 同步回读，不应出现在正常渲染路径上。
#ifndef MOLECULENS_RENDER_DIAG
#define MOLECULENS_RENDER_DIAG 0
#endif

namespace moleculens::app {

namespace {

// CPU 光追相机参数（视口预览与导出共用；此前两处各写一遍同样的 8 行）
engine::RtCamera rtCameraFrom(const engine::OrbitCamera& cam, uint32_t w, uint32_t h) {
    engine::RtCamera rtc;
    rtc.position = cam.position();
    rtc.target = cam.target;
    // 相机 up（含滚转）：视图矩阵第二列（世界空间，与 pan 提取 right 同法）
    const glm::mat4 V = cam.viewMatrix();
    rtc.up = glm::normalize(glm::vec3(V[0][1], V[1][1], V[2][1]));
    rtc.fovDeg = cam.fov;
    rtc.width = w;
    rtc.height = h;
    return rtc;
}

} // namespace

MolScene::MolScene() = default;

MolScene::~MolScene() { shutdown(); }

const entity::SceneStructure* MolScene::scene() const {
    return renderer_ ? renderer_->scene() : nullptr;
}

void MolScene::init(engine::Renderer* renderer, engine::OrbitCamera* camera,
                    engine::VulkanRenderer* vk) {

    renderer_ = renderer;
    camera_ = camera;
    // cameraCtl_ 由 ViewControl::svcCamera 在控制器装配阶段补注入（见头文件说明）
    offscreen_ = std::make_unique<engine::OffscreenRenderer>();
    std::string err;

    // 复用主设备：离屏 FBO 图像创建在主设备上，ImGui 描述符集（主设备 imgui 池）
    // 才能采样到它。若用独立 headless 设备，colorView 属另一 VkDevice、主渲染通道
    // 无法采样 → 视口必然空白（见 moleculens-fixes）。
    // 因此这里**不设 headless 兜底**：那条路径已知不可能工作，只会把失败推迟到
    // 运行期表现为"视口空白"，不如在初始化处直接失败并留下明确日志。
    if (!vk || !offscreen_->initShared(vk->device(), 640, 480, &err)) {
        spdlog::error("[ui] viewport offscreen init failed (shared device): {}", err);
        offscreen_.reset();
        return;
    }
    spdlog::info("[ui] viewport offscreen initialized on main device (640x480)");
}

void MolScene::shutdown() {
    destroyRtTexture();
    exportTarget_.reset();   // 其 device 取自 offscreen_，须先于 offscreen_ 释放
    offscreen_.reset();
    texture_ = ImTextureID(0);
}

uint64_t MolScene::cameraFingerprint(const engine::OrbitCamera& cam) {
    // FNV-1a：把相机可观测状态的**原始字节**依次混入（按位模式比较，浮点
    // 相等性问题（-0.0/+0.0、NaN）在此表现为"变了就重绘"，只会多画不会漏画）
    uint64_t h = 1469598103934665603ull;   // FNV offset basis
    const auto mix = [&h](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;         // FNV prime
        }
    };
    mix(&cam.target, sizeof(cam.target));
    mix(&cam.yaw, sizeof(cam.yaw));
    mix(&cam.pitch, sizeof(cam.pitch));
    mix(&cam.distance, sizeof(cam.distance));
    mix(&cam.roll, sizeof(cam.roll));
    mix(&cam.fov, sizeof(cam.fov));
    mix(&cam.ortho, sizeof(cam.ortho));
    mix(&cam.orthoScale, sizeof(cam.orthoScale));
    return h;
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
        // FBO 重建 → 纹理需重建；先释放旧描述符，否则窗口 resize 期间每帧泄漏一个
        releaseDescriptor(texture_);
    }

    // 相机移动 / 场景脏 → 重渲染
    // 除 yaw/pitch/distance/target 外还须检测 fov/ortho（工具栏切换视角）与
    // 场景脏位（背景/表示变更），否则改了视角/背景视图不刷新。
    const uint64_t camFp = cameraFingerprint(*camera_);
    const bool cameraMoved = (camFp != lastCameraFp_);
    const bool sceneDirty = renderer_->scene()->dirtyBits() != 0;

    // 判断变化 是否需要渲染器更新
    if (dirty_ || cameraMoved || sceneDirty ||
        renderer_->commandSerial() != lastSerial_) {

        renderer_->update(0.0f);   // 脏位域 → needsRebuild_（prepare 阶段重建命令流）
        const auto& props = renderer_->scene()->properties();
        offscreen_->setBackground(props.bgColor);
        renderer_->prepare();

#if MOLECULENS_RENDER_DIAG
        // 诊断（几何数量变化时）：数据 → 命令流是否确实生成了几何
        {
            engine::CommandStats st;
            engine::commandStats(renderer_->commandList(), &st);
            static uint32_t sLast = UINT32_MAX;
            if (st.spheres != sLast) {
                sLast = st.spheres;
                spdlog::info("[ui] viewport command list: {} spheres, {} cylinders, {} lines, {} meshes, serial={}",
                             st.spheres, st.cylinders, st.lines, st.meshes,
                             renderer_->commandSerial());
            }
        }
#endif

        // 一次渲染所需的完整视图参数（相机 + 物理分辨率 + 场景光照/雾）
        engine::RenderView view = renderer_->viewFor(*camera_, w, h);
        /// @key 渲染的关键
        offscreen_->execute(renderer_->commandList(), view);

#if MOLECULENS_RENDER_DIAG
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
#endif   // MOLECULENS_RENDER_DIAG
        lastSerial_ = renderer_->commandSerial();
        lastCameraFp_ = camFp;
        dirty_ = false;
    }
    if (texture_ == ImTextureID(0) && offscreen_->colorView() != VK_NULL_HANDLE) {
        // vcpkg imgui 的 AddTexture 返回 VkDescriptorSet（旧版签名）→ 转 ImTextureID
        const VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
            offscreen_->sampler(), offscreen_->colorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        texture_ = ImTextureID(reinterpret_cast<uintptr_t>(desc));
        // debug 级：resize 期间每帧都会走到这里，info 级会刷屏（每帧一次）
        spdlog::debug("[ui] viewport texture descriptor created (image {}x{}, layout={})",
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
    const uint64_t camFp = cameraFingerprint(*camera_);
    const bool cameraMoved = (camFp != lastCameraFp_);
    const bool sceneDirty = renderer_->scene()->dirtyBits() != 0;
    if (dirty_ || cameraMoved || sceneDirty ||
        renderer_->commandSerial() != lastSerial_) {
        renderer_->update(0.0f);
        renderer_->prepare();
        engine::RtScene rtScene;
        engine::buildRtScene(renderer_->commandList(), rtScene);
        engine::RtCamera rtc = rtCameraFrom(*camera_, rw, rh);

        // 光照与光栅化一致（直接取场景光照，无需构造 RenderView）
        const engine::LightParams light = renderer_->sceneLighting();
        engine::RtLight rtl;
        rtl.skyColor = light.skyColor;
        rtl.groundColor = light.groundColor;
        rtl.ambientColor = light.ambientColor;
        rtl.ambientIntensity = light.ambientIntensity;
        rtl.maxBounces = 0;        // 预览：无反射（保证交互帧率）

        const auto& props = renderer_->scene()->properties();
        const glm::vec3 bg(props.bgColor[0], props.bgColor[1], props.bgColor[2]);
        std::vector<uint8_t> rgba;
        engine::rayTrace(rtScene, rtc, rtl, bg, 1, rgba);
        updateRtTexture(rgba);

        lastSerial_ = renderer_->commandSerial();
        lastCameraFp_ = camFp;
        dirty_ = false;
    }
    if (rtTexture_) {
        ImGui::Image(rtTexture_, size);
    } else {
        ImGui::Text("RT 预览不可用");
    }
}

bool MolScene::handleMouse(float dx, float dy, int button) {
    if (!camera_ || !cameraCtl_) {
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
    // 一律经 CameraController 转发 —— 它是改变相机的唯一入口（不得再直接写 camera_）
    const float sensitivity = 0.008f;
    if (button == 0) {
        // 左键轨道：视角跟随鼠标拖拽方向同步转动。
        //  水平 +dx：拖右 → yaw 增 → 相机绕到 +X（场景右侧），视角转向右侧；
        //  竖直 -dy：拖上（dy<0）→ pitch 增 → 相机升高，看到顶面。
        cameraCtl_->orbit(dx * sensitivity, -dy * sensitivity);
    } else if (button == 1) {
        cameraCtl_->dolly(1.0f + dy * sensitivity);
    } else if (button == 2) {
        const bool shift = ImGui::GetIO().KeyShift;
        if (!shift) {
            // 右键：绕屏幕法线（指向屏幕外）滚转，顺时针/逆时针随拖拽方向
            // （正 roll = 逆时针，见 OrbitCamera::rollView 注释；拖右→顺时针）
            cameraCtl_->rollView(-dx * 0.004f);
        } else {
            // Shift+右键：屏幕空间平移（保留平移能力）
            const float w = static_cast<float>(std::max(1u, displayW_));
            const float h = static_cast<float>(std::max(1u, displayH_));
            cameraCtl_->pan(dx / w * 2.0f, -dy / h * 2.0f);
        }
    }
    return true;
}

void MolScene::handleScroll(float scrollY) {
    if (!cameraCtl_) {
        return;
    }
    // 滚轮缩放（主方案 §7.10：1 - scroll*0.15）→ 同样经控制器
    cameraCtl_->dolly(1.0f - scrollY * 0.15f);
}

bool MolScene::exportToPng(uint32_t width, uint32_t height,
                           const std::string& path, const float bgOverride[3],
                           int ssaa, int colorDepth, bool transparentBg) {
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
    // 内存防护：8K(7680×4320) + SSAA4 的离屏缓冲约 2.1 GB，此前无任何上限检查，
    // 会直接把进程推向 OOM。此处给一个明确上限并报出实际需要量，而不是任其失败。
    constexpr uint64_t kMaxExportBytes = 1ull << 31;   // 2 GiB（含回读副本则 ×2）
    const uint64_t frameBytes = static_cast<uint64_t>(rw) * rh * 4u;
    if (frameBytes > kMaxExportBytes) {
        spdlog::error("[io] export: {}x{} (SSAA {}) 需 {} MiB 离屏缓冲，超出上限 {} MiB"
                      " —— 请降低分辨率或超采样倍数",
                      rw, rh, ss, frameBytes >> 20, kMaxExportBytes >> 20);
        return false;
    }
    // 确保命令流最新（脏则重建）
    renderer_->update(0.0f);
    renderer_->prepare();
    // 常驻导出渲染器（共享主设备；避免改动视口自身 FBO）。
    // 首次创建后复用，省去每次导出重编 10 条管线 + 重建共享网格的开销；
    // 导出可能来自后台线程，故从创建到回读全程持锁串行化。
    std::lock_guard<std::mutex> exportLock(exportMutex_);
    std::string err;
    if (!exportTarget_) {
        exportTarget_ = std::make_unique<engine::OffscreenRenderer>();
        if (!exportTarget_->initShared(offscreen_->device(), rw, rh, &err)) {
            spdlog::error("[io] export: offscreen init failed: {}", err);
            exportTarget_.reset();
            return false;
        }
    } else if (exportTarget_->width() != rw || exportTarget_->height() != rh) {
        exportTarget_->resize(rw, rh);
    }
    const auto& props = renderer_->scene()->properties();
    // 透明背景：清屏 alpha 置 0，几何由不透明管线写入自身 alpha（=1）
    const float bgAlpha = transparentBg ? 0.0f : 1.0f;
    if (bgOverride) {
        exportTarget_->setBackground(bgOverride, bgAlpha);
    } else {
        exportTarget_->setBackground(props.bgColor, bgAlpha);
    }
    engine::RenderView view = renderer_->viewFor(*camera_, rw, rh);
    exportTarget_->execute(renderer_->commandList(), view);
    std::vector<uint8_t> px;
    if (!exportTarget_->readbackPixels(px, engine::PixelFormat::Rgba8)) {
        spdlog::error("[io] export: 像素回读失败");
        return false;
    }
    // 16-bit 输出：在浮点上求平均后再量化到 16 bit。
    // 这是 16-bit 唯一**真实**有意义的来源 —— N 个 8-bit 样本的平均值携带超过
    // 8 bit 的信息量，直接量化回 8 bit 会丢弃它（ss=1 时无超采样，16-bit 只是
    // 容器变宽，无额外精度，UI 侧已注明）。
    if (colorDepth == 16) {
        std::vector<uint16_t> out16(static_cast<size_t>(width) * height * 4u);
        const float inv = 1.0f / static_cast<float>(ss * ss);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int dy = 0; dy < ss; ++dy) {
                    for (int dx = 0; dx < ss; ++dx) {
                        const size_t s = (static_cast<size_t>(y * ss + dy) * rw +
                                          static_cast<size_t>(x * ss + dx)) * 4u;
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += static_cast<float>(px[s + c]);
                        }
                    }
                }
                const size_t d = (static_cast<size_t>(y) * width + x) * 4u;
                for (int c = 0; c < 4; ++c) {
                    // 8-bit 区间 [0,255] 映射到 16-bit [0,65535]：乘 257
                    const float v = acc[c] * inv * 257.0f;
                    out16[d + c] =
                        static_cast<uint16_t>(std::clamp(v, 0.0f, 65535.0f) + 0.5f);
                }
            }
        }
        if (!engine::writePng16(path, width, height, out16)) {
            spdlog::error("[io] export: PNG(16-bit) 编码/写入失败");
            return false;
        }
        return true;
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
    engine::RtCamera rtc = rtCameraFrom(*camera_, width, height);

    // 光照与光栅化一致（直接取场景光照，无需构造 RenderView）
    const engine::LightParams light = renderer_->sceneLighting();
    engine::RtLight rtl;
    rtl.skyColor = light.skyColor;
    rtl.groundColor = light.groundColor;
    rtl.ambientColor = light.ambientColor;
    rtl.ambientIntensity = light.ambientIntensity;
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

void MolScene::releaseDescriptor(ImTextureID& tex) {
    if (tex != ImTextureID(0)) {
        ImGui_ImplVulkan_RemoveTexture(
            reinterpret_cast<VkDescriptorSet>(tex));
        tex = ImTextureID(0);
    }
}

void MolScene::releaseImGuiTextures() {
    releaseDescriptor(texture_);
    releaseDescriptor(rtTexture_);
}

bool MolScene::ensureRtTexture(uint32_t w, uint32_t h) {
    if (rtImage_ != VK_NULL_HANDLE && rtW_ == w && rtH_ == h) {
        return true;
    }
    releaseDescriptor(rtTexture_);   // 同视口纹理：不释放会在每次 RT 尺寸变化时泄漏
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
