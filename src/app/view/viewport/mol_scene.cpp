// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.cpp —— 3D 视口实现
// 规格来源：主方案 §7.7（FBO 方案：场景渲染进离屏 FBO，纹理直接给 ImGui 采样）
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/viewport/mol_scene.h"

#include <algorithm>
#include <chrono>
#include <cfloat>   // FLT_MAX（叠加层文字测量 CalcTextSizeA）
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

#include <imgui_impl_vulkan.h>

#include "app/controller/camera/camera_controller.h"
#include "app/view/theme/ui_theme.h"   // fontTitle()：叠加层大字号文字的底模字体
#include "engine/rep/representer.h"    // 白底导出：压暗氢（setLightBackground）
#include "engine/camera/camera.h"
#include "engine/export/overlay/annotation/scene_overlay_raster.h"   // 标注栅格化（PNG 导出）
#include "engine/export/overlay/esp_overlay_raster.h"   // ESP 色标/极值栅格化（PNG 导出）
#include "engine/export/png/png_encoder.h"
#include "engine/raytrace/ray_tracer.h"
#include "engine/render/renderer.h"
#include "engine/style/espsurface/esp_surface.h"   // EspOverlay / espOverlaySnapshot
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

// —— ESP 叠加（色标条 + 极值点数值）——
// 数据源统一是 engine::espOverlaySnapshot()（Renderer 重建命令流时由
// drawEspSurface 写入；无 ESP 表面时 active=false）。视口用 ImGui draw list
// 立即绘制，PNG 导出用 engine/export/overlay 的栅格器写 RGBA8。

// 背景明暗 → 叠加层文字颜色（暗底白字 / 亮底黑字；与栅格化同一判据）
bool backgroundIsDark(const float rgb[3]) {
    const float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    return lum < 0.5f;
}

// 世界坐标 → 屏幕坐标：先按**渲染分辨率**投影（与 GPU 使用同一 viewProj），
// 再按 image 的屏幕矩形换算 —— 显示尺寸与渲染分辨率不同时位置才正确。
bool projectWorldToScreen(const glm::mat4& viewProj, const glm::vec3& world,
                          uint32_t renderW, uint32_t renderH, const ImVec2& imageMin,
                          const ImVec2& imageSize, ImVec2& out) {
    float px = 0.0f;
    float py = 0.0f;
    if (!engine::esp_overlay::projectToPixel(viewProj, world, static_cast<int>(renderW),
                                            static_cast<int>(renderH), px, py)) {
        return false;
    }
    const float sx = imageSize.x / static_cast<float>(std::max(1u, renderW));
    const float sy = imageSize.y / static_cast<float>(std::max(1u, renderH));
    out = ImVec2(imageMin.x + px * sx, imageMin.y + py * sy);
    return true;
}

// —— 叠加层文字绘制 ——
// 参考图观感：字号明显更大 + 全方向描边；显式字号用 drawList->AddText(font, size, …)，
// 底模取主题的 20px 标题字体（避免从 16px 默认字体放大导致糊）。
void drawOutlinedText(ImDrawList* dl, ImFont* font, float fontSize, const ImVec2& pos,
                      ImU32 textColor, ImU32 outlineColor, const char* text) {
    // 描边粗细随字号：1~2.5px（大字号下 1px 显单薄）
    const float o = std::clamp(fontSize * 0.07f, 1.0f, 2.5f);
    // 先画 8 方向偏移的描边层，再画主色：深浅背景 / 任意表面颜色上都清晰
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            dl->AddText(font, fontSize,
                        ImVec2(pos.x + static_cast<float>(dx) * o,
                               pos.y + static_cast<float>(dy) * o),
                        outlineColor, text);
        }
    }
    dl->AddText(font, fontSize, pos, textColor, text);
}

// 指定字号下的文字尺寸（用于标题居中 / 刻度右对齐 / 标签去重叠）
ImVec2 measureTextAt(ImFont* font, float fontSize, const char* text) {
    return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
}

// 把文字左上角夹回 image 内；文字比可用区域还大时直接左/上对齐
// （避免 std::clamp 的 low > high 未定义行为——极小视口下标题可能比图还宽）。
float clampTextLeft(float desired, float minLeft, float maxRight, float textW) {
    const float hi = maxRight - textW;
    return hi <= minLeft ? minLeft : std::clamp(desired, minLeft, hi);
}

float clampTextTop(float desired, float minTop, float maxBottom, float textH) {
    const float hi = maxBottom - textH;
    return hi <= minTop ? minTop : std::clamp(desired, minTop, hi);
}

// 视口内绘制 ESP 叠加：右侧竖直色标条（大字号标题 + 6 刻度 + 底板/外框）
// + 极值点圆点/引导线/数值。取值规则对齐栅格化实现（上=vmax、invert 一致、
// 极值按 |值| 降序放置并去重叠）；字号按视口显示高度百分比给定（任务规格）。
void drawEspOverlayInViewport(const engine::OrbitCamera& cam, uint32_t renderW,
                              uint32_t renderH, const ImVec2& imageMin,
                              const ImVec2& imageSize, bool darkBackground) {
    if (renderW == 0 || renderH == 0 || imageSize.x < 24.0f || imageSize.y < 24.0f) {
        return;
    }
    const engine::EspOverlay ov = engine::espOverlaySnapshot();
    if (!ov.active) {
        return;   // 无 ESP 表面：什么都不画（安全路径）
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dl == nullptr) {
        return;
    }
    // 底模字体：优先 20px 标题字体（显式字号下更锐利），未加载时回落当前字体
    ImFont* font = UITheme::fontTitle();
    if (font == nullptr) {
        font = ImGui::GetFont();
    }
    if (font == nullptr) {
        return;
    }
    const glm::mat4 vp =
        cam.viewProjMatrix(static_cast<float>(renderW) / static_cast<float>(renderH));

    // —— 字号：视口显示高度的百分比（任务规格）+ 合理性上下限 ——
    const float viewH = imageSize.y;
    const float fontTitle = std::clamp(viewH * 0.022f, 15.0f, 40.0f);
    const float fontTick = std::clamp(viewH * 0.014f, 11.0f, 26.0f);
    const float fontExtrema = std::clamp(viewH * 0.019f, 13.0f, 34.0f);

    // —— 配色（随背景明暗）：暗底白字+黑描边 / 亮底黑字+白描边 ——
    const ImU32 textColor = darkBackground ? IM_COL32(246, 246, 246, 255)
                                           : IM_COL32(18, 18, 18, 255);
    const ImU32 outlineColor = darkBackground ? IM_COL32(0, 0, 0, 230)
                                              : IM_COL32(255, 255, 255, 235);
    const ImU32 lineColor = darkBackground ? IM_COL32(240, 240, 240, 185)
                                           : IM_COL32(25, 25, 25, 185);
    const ImU32 panelFill = darkBackground ? IM_COL32(0, 0, 0, 120)
                                           : IM_COL32(255, 255, 255, 170);
    const ImU32 panelBorder = darkBackground ? IM_COL32(240, 240, 240, 150)
                                             : IM_COL32(30, 30, 30, 150);

    // 裁剪到 image 矩形：标签投影到边缘时不会画到视口外面（盖住其它面板）
    dl->PushClipRect(imageMin,
                     ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y), true);

    // —— 色标条（右侧竖条 + 半透明底板 + 外框 + 6 刻度短线与数值）——
    if (ov.vmaxKcal - ov.vminKcal > 1e-6f) {
        // 条宽 ≈ 视口宽 1.2%（最小 14px，超大视口封顶 34px 避免喧宾夺主）
        const float barW = std::max(14.0f, std::min(imageSize.x * 0.012f, 34.0f));
        const float barH = imageSize.y * 0.55f;
        const float pad = std::max(5.0f, fontTick * 0.55f);
        const float barX = imageMin.x + imageSize.x - imageSize.x * 0.105f;  // 左边界
        const float barY = imageMin.y + (imageSize.y - barH) * 0.5f;
        // 底板 + 外框（图例区自成一块，参考图观感）
        dl->AddRectFilled(ImVec2(barX - pad, barY - pad),
                          ImVec2(barX + barW + pad, barY + barH + pad), panelFill, 4.0f);
        dl->AddRect(ImVec2(barX - pad, barY - pad),
                    ImVec2(barX + barW + pad, barY + barH + pad), panelBorder, 4.0f, 0,
                    1.0f);
        // 渐变（上=vmax、下=vmin；invert 与表面着色一致）
        const auto colorAt = [&ov](float frac) -> ImU32 {
            const float value = ov.vmaxKcal + (ov.vminKcal - ov.vmaxKcal) * frac;
            float t = engine::esp_overlay::valueToT(value, ov.vminKcal, ov.vmaxKcal);
            if (ov.invert) {
                t = 1.0f - t;
            }
            const glm::vec3 c =
                glm::clamp(engine::espColormapColor(ov.colormap, t), glm::vec3(0.0f),
                           glm::vec3(1.0f));
            return IM_COL32(static_cast<int>(c.r * 255.0f),
                            static_cast<int>(c.g * 255.0f),
                            static_cast<int>(c.b * 255.0f), 255);
        };
        constexpr int kSegments = 96;
        for (int i = 0; i < kSegments; ++i) {
            const float f0 = static_cast<float>(i) / kSegments;
            const float f1 = static_cast<float>(i + 1) / kSegments;
            const ImU32 c0 = colorAt(f0);
            const ImU32 c1 = colorAt(f1);
            dl->AddRectFilledMultiColor(ImVec2(barX, barY + f0 * barH),
                                        ImVec2(barX + barW, barY + f1 * barH), c0, c0, c1,
                                        c1);
        }
        // 条自身 1px 外框
        dl->AddRect(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), panelBorder,
                    0.0f, 0, 1.0f);
        // 标题（画在底板上方；贴边时向内夹紧）
        const char* title = "ESP (kcal/mol)";
        const ImVec2 titleSize = measureTextAt(font, fontTitle, title);
        const float titleX = clampTextLeft(
            barX + barW * 0.5f - titleSize.x * 0.5f, imageMin.x + 2.0f,
            imageMin.x + imageSize.x - 2.0f, titleSize.x);
        const float titleY = std::max(imageMin.y + 2.0f, barY - pad - titleSize.y - 3.0f);
        drawOutlinedText(dl, font, fontTitle, ImVec2(titleX, titleY), textColor,
                         outlineColor, title);
        // 6 个刻度：短线 + 数值（上=vmax）；数值右对齐到短线左侧
        const float tickLen = std::max(6.0f, fontTick * 0.6f);
        char buf[32];
        for (int i = 0; i < 6; ++i) {
            const float frac = static_cast<float>(i) / 5.0f;
            const float value = ov.vmaxKcal + (ov.vminKcal - ov.vmaxKcal) * frac;
            const float y = barY + frac * (barH - 1.0f);
            dl->AddLine(ImVec2(barX - tickLen, y), ImVec2(barX - 1.0f, y), lineColor,
                        1.0f);
            std::snprintf(buf, sizeof(buf), "%+.1f", value);
            const ImVec2 ts = measureTextAt(font, fontTick, buf);
            const float tx = std::max(barX - tickLen - 4.0f - ts.x, imageMin.x + 2.0f);
            drawOutlinedText(dl, font, fontTick, ImVec2(tx, y - ts.y * 0.5f), textColor,
                             outlineColor, buf);
        }
    }

    // —— 极值点：圆点 + 引导线 + 数值（按 |值| 降序放置，标签矩形去重叠）——
    if (!ov.extrema.empty()) {
        std::vector<const engine::EspExtremum*> ordered;
        ordered.reserve(ov.extrema.size());
        for (const auto& ex : ov.extrema) {
            ordered.push_back(&ex);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return std::fabs(a->valueKcal) > std::fabs(b->valueKcal);
        });
        // 圆点/引导线/去重叠间距均按字号缩放（视口越大，标签越大且越不容易挤）
        const float dotR = std::max(4.5f, fontExtrema * 0.30f);
        const float lead = std::max(10.0f, fontExtrema * 0.80f);
        // 已放置标签的屏幕矩形（x, y, w, h）；重叠则跳过（优先保留 |值| 大的）
        std::vector<ImVec4> placed;
        for (const engine::EspExtremum* ex : ordered) {
            ImVec2 pos;
            if (!projectWorldToScreen(vp, ex->position, renderW, renderH, imageMin,
                                      imageSize, pos)) {
                continue;
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", ex->valueKcal);
            const ImVec2 ts = measureTextAt(font, fontExtrema, buf);
            // 标签默认放圆点右侧；右侧放不下则放左侧（引导线相应反向）
            const bool placeRight =
                pos.x + dotR + lead + ts.x <= imageMin.x + imageSize.x - 4.0f;
            float textX =
                placeRight ? (pos.x + dotR + lead) : (pos.x - dotR - lead - ts.x);
            // 极端贴边时把标签夹回 image 内（宁可压住圆点，也不画出视口）
            textX = clampTextLeft(textX, imageMin.x + 2.0f,
                                  imageMin.x + imageSize.x - 2.0f, ts.x);
            const float lineX0 = placeRight ? (pos.x + dotR) : (pos.x - dotR);
            const float lineX1 = placeRight ? (textX - 3.0f) : (textX + ts.x + 3.0f);
            const float textY = clampTextTop(pos.y - ts.y * 0.5f, imageMin.y + 2.0f,
                                             imageMin.y + imageSize.y - 2.0f, ts.y);
            const ImVec4 rect(textX - 3.0f, textY - 2.0f, ts.x + 6.0f, ts.y + 4.0f);
            bool tooClose = false;
            for (const ImVec4& r : placed) {
                if (rect.x < r.x + r.z && rect.x + rect.z > r.x && rect.y < r.y + r.w &&
                    rect.y + rect.w > r.y) {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) {
                continue;
            }
            placed.push_back(rect);
            // 极大=金、极小=浅蓝（与栅格化版同配色）+ 深色描边（圆点用 24 段，边界圆滑）
            const ImU32 dot = ex->isMaximum ? IM_COL32(255, 205, 60, 250)
                                            : IM_COL32(160, 205, 255, 250);
            dl->AddCircleFilled(pos, dotR, dot, 24);
            dl->AddCircle(pos, dotR, IM_COL32(0, 0, 0, 220), 24,
                          std::max(1.0f, dotR * 0.28f));
            // 引导线：圆点边缘 → 文字
            dl->AddLine(ImVec2(lineX0, pos.y), ImVec2(lineX1, textY + ts.y * 0.5f),
                        lineColor, 1.0f);
            drawOutlinedText(dl, font, fontExtrema, ImVec2(textX, textY), textColor,
                             outlineColor, buf);
        }
    }

    dl->PopClipRect();
}

// PNG 导出：把 ESP 色标条 + 极值数值栅格化进 RGBA8 缓冲（写 PNG 之前调用）。
// 透明 alpha 由调用方决定是否跳过（栅格器只写 RGB，alpha=0 的像素叠加后不可见）。
void applyEspOverlayToRgba(std::vector<uint8_t>& rgba, int w, int h,
                           const glm::mat4& viewProj, bool darkBackground) {
    const engine::EspOverlay ov = engine::espOverlaySnapshot();
    if (!ov.active) {
        return;   // 无 ESP 表面：导出图与原行为一致
    }
    engine::esp_overlay::drawEspOverlay(rgba, w, h, viewProj, ov, darkBackground,
                                        /*drawBar=*/true, /*drawLabels=*/true);
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

void MolScene::render(const ImVec2& size, uint32_t sceneDirtyHint) {

    if (!renderer_ || !camera_ || !offscreen_ || !renderer_->scene()) {
        ImGui::Text(tr("3D 视口未初始化（先打开文件）"));
        return;
    }

    // 记录逻辑显示尺寸（拖拽平移用：屏幕像素 ↔ NDC 换算须用逻辑尺寸）
    displayW_ = static_cast<uint32_t>(std::max(1.0f, size.x));
    displayH_ = static_cast<uint32_t>(std::max(1.0f, size.y));
    // 每帧兜底重置拖拽状态：无任何鼠标键按下时（release 帧可能无位移，main_view
    // 不转发 → handleMouse 不被调用 → 拖拽状态残留 → 后续手势/换键全部失效）。
    // 另外：拖拽键自己已经抬起（即使别的键还按着）也必须解锁，否则"左键点一下
    // 再按右键拖"会被旧的 dragButton_=左键 一直挡住。
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) {
        isDragging_ = false;
        dragButton_ = -1;
    } else if (dragButton_ >= 0 && dragButton_ <= 2 && !ImGui::IsMouseDown(dragButton_)) {
        isDragging_ = false;
        dragButton_ = -1;
    }

    // —— 播放期动态细节 ——
    // 坐标段版本一变（轨迹播放 / 拖时间轴 / 拖动体系）就进入 350 ms 的"动态窗口"：
    // 低细分球、低段数圆柱、跳过阴影 pass。静止后自动恢复高画质（并强制重绘一帧，
    // 否则门控关闭会一直停留在低模画面）。
    const auto nowTp = std::chrono::steady_clock::now();
    const uint64_t coordsVer =
        renderer_ && renderer_->scene() ? renderer_->scene()->coordsSoA().version : 0;
    if (coordsVer != lastCoordsVersion_) {
        lastCoordsVersion_ = coordsVer;
        dynamicDetailUntil_ = nowTp + std::chrono::milliseconds(350);
    }
    const bool dynamicDetail = nowTp < dynamicDetailUntil_;
    if (lastDynamicDetail_ && !dynamicDetail) {
        dirty_ = true;   // 退出低模窗口后重画一帧高清图
    }
    lastDynamicDetail_ = dynamicDetail;
    // 运动窗口内冻结 QuickSurf/vdW 表面几何：轨迹每帧改坐标，若按原逻辑生成表面
    // 会 700+ ms/帧（实测），播放直接掉到 1–3 FPS。球棍仍逐帧动，表面暂停刷新；
    // 窗口结束（暂停 350 ms）后 Renderer 解冻并重建一次当前帧表面。
    if (renderer_ != nullptr) {
        renderer_->setSurfaceFreeze(dynamicDetail);
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
    // 动态分辨率：播放/拖动窗口内再降一档（0.7×），停手 350 ms 后恢复。
    // 只改离屏 FBO 尺寸，ImGui::Image 仍按逻辑 size 拉伸显示，因此相机比例、
    // 拾取坐标与叠加层都不需要改。
    const float dynamicFactor =
        (dynamicResolution_ && dynamicDetail) ? 0.70f : 1.0f;
    const float effectiveScale = std::clamp(renderScale_ * dynamicFactor, 0.35f, 2.0f);
    const uint32_t w = static_cast<uint32_t>(
        std::max(1.0f, size.x * fsx * effectiveScale));
    const uint32_t h = static_cast<uint32_t>(
        std::max(1.0f, size.y * fsy * effectiveScale));

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
    const bool sceneDirty = renderer_->scene()->dirtyBits() != 0 || sceneDirtyHint != 0;
    const bool gateOpen = dirty_ || cameraMoved || sceneDirty ||
                          renderer_->commandSerial() != lastSerial_ ||
                          // 异步表面在后台生成时保持出帧：否则界面空闲后 prepare 不再被调用，
                          // 完成通知没人消费 → 表面要等下一次鼠标/相机变化才出现。
                          renderer_->surfaceGenerationPending();

    // 诊断：原子数变化时（= 导入/替换场景）打出重绘门控的四个输入。
    // 视口空白时这一条能直接指出成因：门控全假 → 永不重绘；或原子数为 0 →
    // 数据没进到渲染器看到的那个场景。
    {
        static size_t sLastAtoms = SIZE_MAX;
        const size_t nAtoms = renderer_->scene()->coordsSoA().positions.size();
        if (nAtoms != sLastAtoms) {
            sLastAtoms = nAtoms;
            spdlog::debug("[ui] viewport scene: atoms={} reps={} dirty=0x{:x} "
                          "cameraMoved={} gateOpen={} serial={} lastSerial={}",
                          nAtoms, renderer_->representations().size(),
                          renderer_->scene()->dirtyBits(), cameraMoved, gateOpen,
                          renderer_->commandSerial(), lastSerial_);
        }
    }

    // 诊断（只在"场景有内容却门控全假"时打印一次）：
    // 该组合意味着视口**永远不会重绘**，表现为"界面正常但视口空白"——
    // 这类静默失效没有崩溃也没有日志，此前极难定位。门控四值一并打出以便定位成因。
    if (!gateOpen && !renderer_->scene()->coordsSoA().positions.empty()) {
        // 仅当"场景有原子，但命令流里一条几何都没有"时才报警 —— 那才是真正的
        // 静默空白（加载了却从没生成过几何）。若命令流已有几何，门控关闭只是
        // "空闲、无变化"，属正常状态，不该误报。
        static bool sWarned = false;
        if (!sWarned) {
            engine::CommandStats st;
            engine::commandStats(renderer_->commandList(), &st);
            if (st.spheres + st.cylinders + st.lines + st.meshes == 0) {
                sWarned = true;
                spdlog::warn("[ui] 场景有原子但命令流无几何 -> 视口不会重绘 "
                             "(dirty_={} cameraMoved={} sceneDirty={} serial={} lastSerial={})",
                             dirty_, cameraMoved, sceneDirty, renderer_->commandSerial(),
                             lastSerial_);
            }
        }
    }

    // 判断变化 是否需要渲染器更新
    if (gateOpen) {
        renderer_->setTrajectoryFrame(trajectoryFrame_);
        // 多视图外层抓到的脏位快照优先：场景脏位可能已被先渲染的视图清掉。
        if (sceneDirtyHint != 0) {
            renderer_->updateWithDirty(0.0f, sceneDirtyHint);
        } else {
            renderer_->update(0.0f);   // 脏位域 → needsRebuild_（prepare 阶段重建命令流）
        }
        const auto& props = renderer_->scene()->properties();
        offscreen_->setBackground(props.bgColor);
        const auto tPrepare0 = std::chrono::steady_clock::now();
        renderer_->prepare();
        const auto tPrepare1 = std::chrono::steady_clock::now();

        // 诊断（命令流重建时打一条）：这是"数据 → 命令流"链路上最有价值的一处观测
        // —— 视口空白时，它能立刻区分"命令流本来就是空的"（生成侧问题）与
        // "命令流有几何但没画出来"（后端/合成问题）。
        // 用 serial 做门控（命令流重建才递增）→ 常态下只比一个整数，不做逐命令遍历。
        {
            static uint64_t sLastSerial = UINT64_MAX;
            const uint64_t ser = renderer_->commandSerial();
            if (ser != sLastSerial) {
                sLastSerial = ser;
                engine::CommandStats st;
                engine::commandStats(renderer_->commandList(), &st);
                spdlog::debug("[ui] viewport command list: {} spheres, {} cylinders, "
                              "{} lines, {} meshes, serial={}",
                              st.spheres, st.cylinders, st.lines, st.meshes, ser);
            }
        }

        // 一次渲染所需的完整视图参数（相机 + 物理分辨率 + 场景光照/雾）
        engine::RenderView view = renderer_->viewFor(*camera_, w, h);
        view.lowDetail = dynamicDetail;
        /// @key 渲染的关键
        // 耗时拆分（每 60 帧一条 debug）：轨迹播放掉帧时能直接区分"命令流重放慢"
        // 与"GPU 执行慢"，不必再猜。
        const auto tPrep0 = std::chrono::steady_clock::now();
        offscreen_->execute(renderer_->commandList(), view);
        const auto tPrep1 = std::chrono::steady_clock::now();
        static int sViewPerf = 0;
        if ((sViewPerf++ % 60) == 0) {
            const double prepareMs =
                std::chrono::duration<double, std::milli>(tPrepare1 - tPrepare0).count();
            const double execMs =
                std::chrono::duration<double, std::milli>(tPrep1 - tPrep0).count();
            if (prepareMs + execMs > 4.0) {
                spdlog::debug("[perf] viewport prepare {:.1f} ms, execute {:.1f} ms "
                              "(lowDetail={})",
                              prepareMs, execMs, dynamicDetail);
            }
        }

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
        // ESP 视口叠加：右侧色标条 + 极值点数值（无 ESP 表面时 active=false → 不画）。
        // 在 Image 之后用 draw list 绘制，投影按 image 的屏幕矩形换算，故窗口缩放/
        // 高 DPI 下位置与表面一致。
        const auto& props = renderer_->scene()->properties();
        const ImVec2 imgMin = ImGui::GetItemRectMin();
        const ImVec2 imgSize = ImGui::GetItemRectSize();
        drawEspOverlayInViewport(*camera_, viewportW_, viewportH_,
                                 imgMin, imgSize,
                                 backgroundIsDark(props.bgColor));
        // 尺寸 / 比例尺 / 坐标轴（用户可分别关闭；不参与布局、不挤动视口）
        drawViewOverlay(imgMin, imgSize);
        // 表面生成中提示：异步模式下 QS 表面在后台生成，期间旧画面继续显示。
        // 文字用视口 draw list 叠在图像左上角，不参与布局（不挤动 3D 视口）。
        if (renderer_->surfaceGenerationPending()) {
            const ImVec2 rectMin = ImGui::GetItemRectMin();
            const ImU32 col = backgroundIsDark(props.bgColor) ? IM_COL32(235, 235, 240, 235)
                                                              : IM_COL32(40, 40, 48, 235);
            ImGui::GetWindowDrawList()->AddText(ImVec2(rectMin.x + 10.0f, rectMin.y + 8.0f),
                                                col, tr("表面生成中…"));
        }
    } else {
        ImGui::Text(tr("视口纹理不可用"));
    }
}

void MolScene::renderRayTracePreview(const ImVec2& size) {
    if (!renderer_ || !camera_ || !renderer_->scene()) {
        return;
    }
    const uint64_t camFp = cameraFingerprint(*camera_);
    const bool cameraMoved = (camFp != lastCameraFp_);
    const bool sceneDirty = renderer_->scene()->dirtyBits() != 0;
    const uint64_t serial = renderer_->commandSerial();
    const bool serialChanged = (serial != lastSerial_);
    const bool needRender = dirty_ || cameraMoved || sceneDirty || serialChanged;

    // —— RT 级 LOD 之一：分辨率两档 ——
    // 交互中（相机在动）按低分辨率追踪，相机停手 kSettleSec 后升级高清档重渲一次
    // （spp 也提到 2），下次交互自动退回低档。
    //
    // 注意：纹理尺寸固定为高档（kCapSettled），低档结果在 CPU 侧最近邻放大后上传。
    // 若随档位改纹理尺寸，就会在每次档位切换时销毁/重建 VkImage，而上一帧的
    // ImGui 绘制仍可能引用旧描述符 —— 实测会刷屏 vkQueueSubmit failed。
    constexpr double kSettleSec = 0.25;
    constexpr uint32_t kCapInteractive = 320;
    constexpr uint32_t kCapSettled = 720;
    const double now = ImGui::GetTime();
    if (needRender) {
        rtLastChangeTime_ = now;
        rtRefined_ = false;
    }
    const bool settled = (now - rtLastChangeTime_) >= kSettleSec;

    // 纹理（固定尺寸）：宽 ≤ kCapSettled，保持宽高比
    uint32_t tw = std::max(1u, displayW_);
    uint32_t th = std::max(1u, displayH_);
    if (tw > kCapSettled) {
        th = std::max(1u, static_cast<uint32_t>(static_cast<float>(th) * kCapSettled / tw));
        tw = kCapSettled;
    }
    if (tw != rtW_ || th != rtH_) {
        if (!ensureRtTexture(tw, th)) {
            ImGui::Text(tr("RT 预览纹理不可用"));
            return;
        }
    }

    if (needRender || (settled && !rtRefined_)) {
        const auto t0 = std::chrono::steady_clock::now();

        // —— RT 级 LOD 之二：场景缓冲复用 ——
        // 只有命令流变化才重建 RtScene + BVH（表面表示 91 万三角形建树 ~119 ms）；
        // 相机移动时直接复用，交互帧只剩光线投射。
        bool sceneRebuilt = false;
        if (!rtCacheValid_ || serialChanged || sceneDirty) {
            renderer_->update(0.0f);
            renderer_->prepareBlocking();   // RT 预览需要完整命令流（异步表面必须先就绪）
            engine::buildRtScene(renderer_->commandList(), rtSceneCache_);
            rtAccelCache_ = engine::buildRtAccel(rtSceneCache_);
            rtCacheSerial_ = renderer_->commandSerial();
            rtCacheValid_ = true;
            sceneRebuilt = true;
        }

        // 追踪分辨率：交互档按比例缩小
        uint32_t rw = tw;
        uint32_t rh = th;
        if (!settled && rw > kCapInteractive) {
            rh = std::max(1u, static_cast<uint32_t>(static_cast<float>(rh) * kCapInteractive / rw));
            rw = kCapInteractive;
        }

        engine::RtCamera rtc = rtCameraFrom(*camera_, rw, rh);

        // 光照与光栅化一致（直接取场景光照，无需构造 RenderView）
        const engine::LightParams light = renderer_->sceneLighting();
        engine::RtLight rtl;
        // 预览：无反射（保证交互帧率）+ 单条阴影光线（硬阴影，够快）
        engine::rtLightFrom(light, rtl, /*shadowSamples=*/1, /*maxBounces=*/0);

        const auto& props = renderer_->scene()->properties();
        const glm::vec3 bg(props.bgColor[0], props.bgColor[1], props.bgColor[2]);
        const int spp = settled ? 2 : 1;
        std::vector<uint8_t> rgba;
        engine::rayTrace(rtSceneCache_, rtc, rtl, bg, spp, rgba, rtAccelCache_);

        // 低档 → 纹理尺寸的最近邻放大（纹理尺寸恒定，避免重建 VkImage）
        if (rw != tw || rh != th) {
            std::vector<uint8_t> up(static_cast<size_t>(tw) * th * 4, 0);
            for (uint32_t y = 0; y < th; ++y) {
                const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * rh / th);
                for (uint32_t x = 0; x < tw; ++x) {
                    const uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(x) * rw / tw);
                    const uint8_t* s = &rgba[(static_cast<size_t>(sy) * rw + sx) * 4];
                    uint8_t* d = &up[(static_cast<size_t>(y) * tw + x) * 4];
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = s[3];
                }
            }
            rgba.swap(up);
        }
        updateRtTexture(rgba);

        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        spdlog::debug("[ui] rt preview: {}x{} spp={} {} ms（{} 三角，场景{}）",
                      rw, rh, spp, ms, rtSceneCache_.triangles.size(),
                      sceneRebuilt ? "重建" : "缓存命中");

        lastSerial_ = renderer_->commandSerial();
        lastCameraFp_ = camFp;
        dirty_ = false;
        rtRefined_ = settled;
    }
    if (rtTexture_) {
        ImGui::Image(rtTexture_, size);
        // ESP 叠加同样作用于 RT 预览（数据来自同一次命令流重建）
        const auto& props = renderer_->scene()->properties();
        drawEspOverlayInViewport(*camera_, rtW_, rtH_, ImGui::GetItemRectMin(),
                                 ImGui::GetItemRectSize(),
                                 backgroundIsDark(props.bgColor));
    } else {
        ImGui::Text(tr("RT 预览不可用"));
    }
}

// 分子显示视图叠加层（v2.12）：
//   右上：场景包围盒尺寸（Å）——回答"这个分子有多大"
//   右下：比例尺（Å）——回答"屏幕上一段有多长"
//   左下：坐标轴方向（X 红 / Y 绿 / Z 蓝）——回答"现在看的是哪个方向"
// 三者都用 draw list 画在图像之上，不参与 ImGui 布局（不会挤动 3D 视口），
// 并且由调用方分别开关（用户要求"可选择关闭"）。
void MolScene::drawViewOverlay(const ImVec2& imgMin, const ImVec2& imgSize) {
    if ((!showAxes_ && !showScaleBar_) || imgSize.x < 80.0f || imgSize.y < 80.0f ||
        renderer_ == nullptr || camera_ == nullptr || renderer_->scene() == nullptr) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& props = renderer_->scene()->properties();
    const bool dark = backgroundIsDark(props.bgColor);
    const ImU32 panelBg = dark ? IM_COL32(10, 14, 22, 188) : IM_COL32(255, 255, 255, 214);
    const ImU32 textCol = dark ? IM_COL32(232, 238, 248, 245) : IM_COL32(26, 34, 48, 245);
    const ImU32 mutedCol = dark ? IM_COL32(176, 188, 206, 225) : IM_COL32(88, 102, 122, 230);
    ImFont* font = ImGui::GetFont();
    const float fs = std::clamp(imgSize.y * 0.024f, 12.0f, 16.0f);

    dl->PushClipRect(imgMin, ImVec2(imgMin.x + imgSize.x, imgMin.y + imgSize.y), true);

    // —— 右上：场景尺寸 + 右下：比例尺 ——
    if (showScaleBar_) {
        const auto& pos = renderer_->scene()->coordsSoA().positions;
        const uint64_t ver = renderer_->scene()->coordsSoA().version;
        if (!pos.empty() && dimsVersion_ != ver) {
            dimsVersion_ = ver;
            dimsMin_ = dimsMax_ = pos.front();
            for (const glm::vec3& p : pos) {
                dimsMin_ = glm::min(dimsMin_, p);
                dimsMax_ = glm::max(dimsMax_, p);
            }
        }
        if (!pos.empty()) {
            const glm::vec3 d = dimsMax_ - dimsMin_;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %.1f × %.1f × %.1f Å", tr("尺寸"),
                          static_cast<double>(d.x), static_cast<double>(d.y),
                          static_cast<double>(d.z));
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
            const ImVec2 tp(imgMin.x + imgSize.x - ts.x - 22.0f, imgMin.y + 12.0f);
            dl->AddRectFilled(ImVec2(tp.x - 7.0f, tp.y - 4.0f),
                              ImVec2(tp.x + ts.x + 7.0f, tp.y + ts.y + 4.0f), panelBg, 5.0f);
            dl->AddText(font, fs, tp, textCol, buf);
        }

        const float worldPerPx = [&]() {
            if (camera_->ortho) {
                return (2.0f * camera_->orthoScale) / std::max(1.0f, imgSize.y);
            }
            const float halfH = std::max(0.01f, camera_->distance) *
                                std::tan(glm::radians(camera_->fov) * 0.5f);
            return (2.0f * halfH) / std::max(1.0f, imgSize.y);
        }();
        if (worldPerPx > 0.0f) {
            const float raw = 110.0f * worldPerPx;
            const float exp10 = std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-9f))));
            const float mant = raw / exp10;
            const float nice =
                mant < 1.5f ? 1.0f : (mant < 3.5f ? 2.0f : (mant < 7.5f ? 5.0f : 10.0f));
            float world = nice * exp10;
            float px = world / worldPerPx;
            if (px > 190.0f) {
                world *= 0.5f;
                px *= 0.5f;
            }
            char label[64];
            std::snprintf(label, sizeof(label), "%.3g Å", static_cast<double>(world));
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
            const float x1 = imgMin.x + imgSize.x - 18.0f;
            const float x0 = x1 - std::max(px, 12.0f);
            const float y = imgMin.y + imgSize.y - 24.0f;
            dl->AddRectFilled(ImVec2(x0 - 8.0f, y - ts.y - 10.0f),
                              ImVec2(x1 + 8.0f, y + 9.0f), panelBg, 5.0f);
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), textCol, 2.0f);
            dl->AddLine(ImVec2(x0, y - 4.0f), ImVec2(x0, y + 4.0f), textCol, 2.0f);
            dl->AddLine(ImVec2(x1, y - 4.0f), ImVec2(x1, y + 4.0f), textCol, 2.0f);
            dl->AddText(font, fs, ImVec2((x0 + x1) * 0.5f - ts.x * 0.5f, y - ts.y - 4.0f),
                        textCol, label);
        }
    }

    // —— 左下：坐标轴方向（把世界 X/Y/Z 投到屏幕；数值就是相机视图的旋转） ——
    if (showAxes_) {
        const glm::mat3 viewRot(camera_->viewMatrix());
        const ImVec2 o(imgMin.x + 48.0f, imgMin.y + imgSize.y - 48.0f);
        const float len = std::clamp(std::min(imgSize.x, imgSize.y) * 0.075f, 18.0f, 34.0f);
        dl->AddCircleFilled(o, len + 12.0f, panelBg, 24);
        struct Axis {
            glm::vec3 dir;
            const char* name;
            ImU32 color;
        };
        const Axis axes[3] = {
            {{1.0f, 0.0f, 0.0f}, "X", IM_COL32(244, 96, 96, 255)},
            {{0.0f, 1.0f, 0.0f}, "Y", IM_COL32(96, 214, 128, 255)},
            {{0.0f, 0.0f, 1.0f}, "Z", IM_COL32(100, 158, 255, 255)},
        };
        for (const Axis& a : axes) {
            const glm::vec3 v = viewRot * a.dir;
            ImVec2 dir(v.x, -v.y);
            const float mag = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (mag < 0.08f) {
                // 轴几乎正对相机：端点退化为圆点，标签放在右上
                dl->AddCircleFilled(ImVec2(o.x + v.x * 4.0f, o.y + v.y * 4.0f), 3.0f,
                                    a.color, 12);
                dl->AddText(font, fs, ImVec2(o.x + 7.0f, o.y - fs - 6.0f), a.color, a.name);
                continue;
            }
            dir.x /= mag;
            dir.y /= mag;
            const ImVec2 tip(o.x + dir.x * len, o.y + dir.y * len);
            dl->AddLine(o, tip, a.color, 2.6f);
            dl->AddCircleFilled(tip, 3.2f, a.color, 12);
            dl->AddText(font, fs,
                        ImVec2(tip.x + dir.x * 7.0f - fs * 0.25f,
                               tip.y + dir.y * 7.0f - fs * 0.55f),
                        a.color, a.name);
        }
        dl->AddText(font, fs * 0.82f, ImVec2(o.x - 12.0f, o.y + len + 10.0f), mutedCol,
                    tr("方向"));
    }

    dl->PopClipRect();
}

bool MolScene::handleMouse(float dx, float dy, int button) {
    if (!camera_ || !cameraCtl_ || button < 0 || button > 2) {
        return false;
    }
    // 按钮已抬起：结束本次拖拽。release 帧可能无位移，调用方只在有输入时转发，
    // 因此还需在 render() 每帧做兜底重置（否则拖拽状态残留 → 后续手势失效）。
    if (!ImGui::IsMouseDown(button)) {
        if (dragButton_ == button) {
            isDragging_ = false;
            dragButton_ = -1;
        }
        return false;
    }
    // 只有真正产生位移才进入拖拽。旧实现"按下即锁定按键"：先点一下左键（无位移）
    // 再按住右键拖动时，右键事件被 dragButton_ 拒绝 —— 用户看到的就是
    // "右键滚转/中键缩放完全用不上"，而左键"偶尔能用"。点击不移动不该锁键。
    if (!isDragging_) {
        if (std::fabs(dx) + std::fabs(dy) <= 0.0f) {
            return false;
        }
        isDragging_ = true;
        dragButton_ = button;
    }
    if (dragButton_ != button) {
        return false;   // 其它按钮的拖拽：忽略（避免多键串扰）
    }
    // 主方案 §7.10：左键轨道旋转 / 中键 dolly / 右键绕屏幕法线滚转（Shift+右键平移）
    // 一律经 CameraController 转发 —— 它是改变相机的唯一入口（不得再直接写 camera_）
    //
    // ⚠ ImGui 的鼠标键编号是 0=左 / 1=右 / 2=中（Vulkan/GLFW 顺序），不是
    //   "中键=1、右键=2"。此前按后者写分支，导致右键实际走 dolly、中键走滚转，
    //   与面板/文档（右键滚转、中键推拉）完全相反 —— 这是"鼠标手势用不上"的
    //   根因之一。下面直接用 ImGuiMouseButton_* 常量，杜绝同类错位。
    const float sensitivity = 0.006f;
    if (button == ImGuiMouseButton_Left) {
        // 左键轨道：**抓住分子拖**的手感（VMD/PyMOL 一致）：
        //   · 水平：拖右 → 相机向左绕（分子右侧转过来），与 v2.13 用户要求一致，
        //     所以 dx 取负；
        //   · 垂直：ImGui 的 dy 向下为正。pitch 增大 = 相机升高（从上方看），
        //     屏幕上分子会**向下**滑。要"分子跟着鼠标走"，拖下（dy>0）就该让
        //     pitch 增大，因此 dy 必须取**正**。此前和 dx 一起取负 → 上下方向
        //     相反，用户反馈"左键手势很奇怪"就是它。
        cameraCtl_->orbit(-dx * sensitivity, dy * sensitivity);
    } else if (button == ImGuiMouseButton_Right) {
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
    } else {   // ImGuiMouseButton_Middle
        // 中键推拉：向上拖拉近、向下拖推远（factor 与 dy 同号：dy>0 距离变大）。
        cameraCtl_->dolly(1.0f + dy * sensitivity);
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
    // 导出线程：先等待后台表面生成完成，再同步重建 —— 导出图不会出现"生成中"的旧画面
    renderer_->prepareBlocking();
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
    // 导出用更高细分：球 5120 面 / 柱 48 段，放大 3–5 倍仍圆；预览档位不受影响
    exportTarget_->setMeshQuality(4, 48);

    // 透明背景：清屏 alpha 置 0，几何由不透明管线写入自身 alpha（=1）
    const float bgAlpha = transparentBg ? 0.0f : 1.0f;
    // 背景明暗（决定 ESP 叠加文字用白字还是黑字；透明背景按白底语义取亮色）
    const float* effBg = bgOverride != nullptr ? bgOverride : props.bgColor;
    const bool darkBg = backgroundIsDark(effBg);
    if (bgOverride) {
        exportTarget_->setBackground(bgOverride, bgAlpha);
    } else {
        exportTarget_->setBackground(props.bgColor, bgAlpha);
    }
    // 白底/透明背景导出：通知配色把氢压暗（与 CLI --bg white 同一策略）。
    // 用作用域守卫，导出结束即恢复，避免影响视口内的正常显示。
    struct LightBgGuard {
        explicit LightBgGuard(bool on) { engine::Representer::setLightBackground(on); }
        ~LightBgGuard() { engine::Representer::setLightBackground(false); }
    } lightBgGuard(!darkBg || transparentBg);

    engine::RenderView view = renderer_->viewFor(*camera_, rw, rh);

    // —— 16-bit 路径：走 16F 半浮点附件（主方案 §12.5）——
    // 16F 是**线性**附件（无硬件 sRGB 编码），回读后由 writePng16FromHalfRgba 完成
    // 色调映射 + sRGB + 16-bit 量化。相对"8-bit 多采样求平均"的实质增益：
    //   ① 求平均在**线性空间**进行（8-bit 路径平均的是 sRGB 编码后的值）
    //   ② 全程不经 8-bit 量化，动态范围不受 [0,1] 限制
    // 设备不支持 16F 颜色附件时退回 8-bit 多采样求平均（下方 colorDepth==16 分支）。
    if (colorDepth == 16 &&
        exportTarget_->setColorAttachment(engine::ColorAttachment::Rgba16Float)) {
        exportTarget_->execute(renderer_->commandList(), view);
        std::vector<uint8_t> halfBytes;
        if (!exportTarget_->readbackPixels(halfBytes, engine::PixelFormat::Rgba16F)) {
            spdlog::error("[io] export: 16F 像素回读失败");
            return false;
        }
        std::vector<uint16_t> half(halfBytes.size() / 2);
        if (!halfBytes.empty()) {
            std::memcpy(half.data(), halfBytes.data(), halfBytes.size());
        }
        if (!engine::writePng16FromHalfRgba(path, width, height, half, rw, rh)) {
            spdlog::error("[io] export: PNG(16-bit/HDR) 编码/写入失败");
            return false;
        }
        spdlog::info("[io] export: {} -> {} ({}x{}, ssaa={}, 16-bit via 16F attachment)",
                     path.empty() ? "(png)" : path, width, height, ss);
        return true;
    }
    if (colorDepth == 16) {
        spdlog::warn("[io] export: 16F 颜色附件不可用 -> 退回 8-bit 多采样求平均");
    }
    // 非 16-bit（或 16F 不可用）→ 8-bit SRGB 附件
    if (!exportTarget_->setColorAttachment(engine::ColorAttachment::Rgba8Srgb)) {
        spdlog::error("[io] export: 无法切回 8-bit SRGB 颜色附件");
        return false;
    }
    exportTarget_->execute(renderer_->commandList(), view);
    std::vector<uint8_t> px;
    if (!exportTarget_->readbackPixels(px, engine::PixelFormat::Rgba8)) {
        spdlog::error("[io] export: 像素回读失败");
        return false;
    }
    // 16-bit 退路：在浮点上对 ss² 个 8-bit 样本求平均后再量化到 16 bit ——
    // N 个样本的均值携带超过 8 bit 的信息量，直接量化回 8 bit 会丢弃它
    //（ss=1 时无超采样，16-bit 只是容器变宽，无额外精度）。
    // 与 8-bit 路径同法：颜色通道先解回线性再平均（否则 gamma 加权使结果偏暗），
    // 再由 linearToSrgb16 编码并直接量化到 16 bit —— 因此这里确实产出了
    // 8-bit 分辨率之外的中间值（求平均保留的亚 byte 信息被 16-bit 容器留住）。
    if (colorDepth == 16) {
        std::vector<uint16_t> out16(static_cast<size_t>(width) * height * 4u);
        const float inv = 1.0f / (static_cast<float>(ss) * static_cast<float>(ss));
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int dy = 0; dy < ss; ++dy) {
                    for (int dx = 0; dx < ss; ++dx) {
                        const size_t s = (static_cast<size_t>(y * ss + dy) * rw +
                                          static_cast<size_t>(x * ss + dx)) * 4u;
                        for (int c = 0; c < 3; ++c) {
                            acc[c] += engine::srgbToLinear(px[s + c] * (1.0f / 255.0f));
                        }
                        // alpha 是线性不透明度（非 sRGB 编码）→ 直接按字节累加
                        acc[3] += static_cast<float>(px[s + 3]);
                    }
                }
                const size_t d = (static_cast<size_t>(y) * width + x) * 4u;
                for (int c = 0; c < 3; ++c) {
                    out16[d + c] = engine::linearToSrgb16(acc[c] * inv);
                }
                // 8-bit 区间 [0,255] 映射到 16-bit [0,65535]：乘 257
                out16[d + 3] = static_cast<uint16_t>(
                    std::clamp(acc[3] * inv * 257.0f, 0.0f, 65535.0f) + 0.5f);
            }
        }
        if (!engine::writePng16(path, width, height, out16)) {
            spdlog::error("[io] export: PNG(16-bit) 编码/写入失败");
            return false;
        }
        return true;
    }
    // 超采样（SSAA）：把 rw×rh 盒式降采样回 width×height → 抗锯齿。
    //
    // 求平均在**线性**空间进行：颜色附件是 Rgba8Srgb（硬件已做 sRGB 编码），
    // 直接在字节值上平均等于按 gamma≈2.2 加权 —— 高亮/暗部两侧的权重都偏离
    // 能量平均，抗锯齿后边缘会系统性偏暗发灰。故先把每个样本经 srgbToLinear
    // 解回线性、平均、再由 linearToSrgb8 编码回去（后者自带 +0.5 四舍五入；
    // 原先的整数除法 r/n 是向下取整，本身也引入约 0.5/255 的系统性偏暗）。
    // 平坦区域经此往返逐字节不变（两函数互为反函数）。
    // alpha 不是 sRGB 编码的（线性不透明度），不能走该往返。
    if (ss > 1) {
        std::vector<uint8_t> outPx(static_cast<size_t>(width) * height * 4);
        const float inv = 1.0f / (static_cast<float>(ss) * static_cast<float>(ss));
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                for (int dy = 0; dy < ss; ++dy) {
                    for (int dx = 0; dx < ss; ++dx) {
                        const size_t s =
                            (static_cast<size_t>(y * ss + dy) * rw +
                             static_cast<size_t>(x * ss + dx)) * 4;
                        r += engine::srgbToLinear(px[s] * (1.0f / 255.0f));
                        g += engine::srgbToLinear(px[s + 1] * (1.0f / 255.0f));
                        b += engine::srgbToLinear(px[s + 2] * (1.0f / 255.0f));
                        a += static_cast<float>(px[s + 3]);
                    }
                }
                const size_t d = (static_cast<size_t>(y) * width + x) * 4;
                // 线性均值必落在 [0,1]（每个样本都已截断到该区间）→ 无需色调映射
                outPx[d] = engine::linearToSrgb8(r * inv);
                outPx[d + 1] = engine::linearToSrgb8(g * inv);
                outPx[d + 2] = engine::linearToSrgb8(b * inv);
                outPx[d + 3] = static_cast<uint8_t>(
                    std::clamp(a * inv, 0.0f, 255.0f) + 0.5f);
            }
        }
        px.swap(outPx);
    }
    // ESP 叠加：色标条 + 极值点数值（与视口叠加同源数据，engine::espOverlaySnapshot）。
    // 透明背景（alpha=0）跳过：栅格器只写 RGB，叠加内容在 alpha=0 的像素上不可见；
    // 16-bit 路径已在上方直接写出（8-bit 栅格器无法叠加到 16F 浮点数据）。
    if (!transparentBg) {
        const glm::mat4 vp = camera_->viewProjMatrix(
            static_cast<float>(width) / static_cast<float>(height));
        applyEspOverlayToRgba(px, static_cast<int>(width), static_cast<int>(height), vp,
                              darkBg);
        // 场景标注（水盒/标签/箭头/水印）：与视口叠加同源数据，导出图里一并出现。
        // 放在 ESP 叠加之后：标签/箭头通常指示具体分子，压住色标更好读。
        const auto annotations = engine::scene_overlay::snapshot();
        engine::scene_overlay::drawSceneOverlay(px, static_cast<int>(width),
                                                static_cast<int>(height), vp, annotations,
                                                darkBg);
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
    renderer_->prepareBlocking();   // 光追导出需要完整命令流（异步表面必须先就绪）
    engine::RtScene rtScene;
    engine::buildRtScene(renderer_->commandList(), rtScene);
    engine::RtCamera rtc = rtCameraFrom(*camera_, width, height);

    // 光照与光栅化一致（直接取场景光照，无需构造 RenderView）
    const engine::LightParams light = renderer_->sceneLighting();
    engine::RtLight rtl;
    // 导出：球体 1 次反射 + 8 次软阴影采样（离线可承受）
    engine::rtLightFrom(light, rtl, /*shadowSamples=*/8, /*maxBounces=*/1);

    const auto& props = renderer_->scene()->properties();
    glm::vec3 bg(props.bgColor[0], props.bgColor[1], props.bgColor[2]);
    if (bgOverride) {
        bg = glm::vec3(bgOverride[0], bgOverride[1], bgOverride[2]);
    }
    std::vector<uint8_t> rgba;
    // 优先 GPU（Vulkan KHR 光追）；设备不支持/运行失败时回退 CPU（结果同契约）。
    bool traced = false;
    if (renderer_->gpuRayTraceAvailable()) {
        const auto t0 = std::chrono::steady_clock::now();
        traced = renderer_->gpuRayTrace(rtScene, rtc, rtl, bg,
                                        std::max(1, samplesPerPixel), rgba);
        if (traced) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            spdlog::info("[io] export(rt): GPU 光追 {}x{} {} spp, {} ms", width, height,
                         std::max(1, samplesPerPixel), ms);
        } else {
            spdlog::warn("[io] export(rt): GPU 光追失败 -> 回退 CPU 光追");
        }
    }
    if (!traced) {
        if (!engine::rayTrace(rtScene, rtc, rtl, bg, std::max(1, samplesPerPixel),
                              rgba)) {
            spdlog::error("[io] export(rt): CPU 光追失败");
            return false;
        }
    }
    // ESP 叠加：与光栅导出同一栅格器/同一份快照（RT 路径无透明背景选项）
    {
        const float bgRgb[3] = {bg.x, bg.y, bg.z};
        const glm::mat4 vp = camera_->viewProjMatrix(
            static_cast<float>(width) / static_cast<float>(height));
        applyEspOverlayToRgba(rgba, static_cast<int>(width), static_cast<int>(height), vp,
                              backgroundIsDark(bgRgb));
    }
    if (!engine::writePng(path, width, height, rgba)) {
        spdlog::error("[io] export(rt): PNG 编码/写入失败");
        return false;
    }
    spdlog::info("[io] export(rt): {} -> {} ({}x{})", path, width, height);
    return true;
}

bool MolScene::exportAnimationToPng(uint32_t width, uint32_t height,
                                    const std::string& dirPath, uint32_t instanceIndex,
                                    uint64_t inFrame, uint64_t outFrame, uint32_t step,
                                    const float bgOverride[3], int ssaa, int colorDepth,
                                    uint32_t* frameCountOut, int source,
                                    uint32_t orbitFrames) {
    if (!renderer_ || renderer_->mutableScene() == nullptr) {
        spdlog::error("[io] export(anim): 渲染器/场景未就绪");
        return false;
    }
    if (!camera_) {
        spdlog::error("[io] export(anim): 相机未就绪");
        return false;
    }
    const bool orbit = (source == 1 || source == 2);
    const engine::OrbitCamera savedCamera = *camera_;
    struct CameraRestore {
        engine::OrbitCamera* cam;
        engine::OrbitCamera saved;
        ~CameraRestore() { *cam = saved; }
    } cameraRestore{camera_, savedCamera};

    std::error_code dirEc;
    std::filesystem::create_directories(dirPath, dirEc);
    if (dirEc) {
        spdlog::error("[io] export(anim): 无法创建输出目录 {}: {}", dirPath,
                      dirEc.message());
        return false;
    }

    // 纯相机旋转：不依赖轨迹，在当前静态场景上绕竖轴转一周。
    if (source == 1) {
        const uint32_t n = std::max(1u, orbitFrames == 0 ? 120u : orbitFrames);
        uint32_t written = 0;
        for (uint32_t i = 0; i < n; ++i) {
            camera_->yaw = savedCamera.yaw +
                           2.0f * 3.14159265358979323846f * static_cast<float>(i) /
                               static_cast<float>(n);
            renderer_->update(0.0f);
            renderer_->prepareBlocking();
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%04u.png", written);
            const std::string out = (std::filesystem::path(dirPath) / name).string();
            if (!exportToPng(width, height, out, bgOverride, ssaa, colorDepth, false)) {
                spdlog::error("[io] export(anim): 相机帧 {} 导出失败，中止", i);
                return false;
            }
            ++written;
        }
        if (frameCountOut != nullptr) {
            *frameCountOut = written;
        }
        spdlog::info("[io] export(anim): 相机旋转 {} 帧 -> {} ({}x{})", written, dirPath,
                     width, height);
        return written > 0;
    }

    entity::SceneStructure* sc = renderer_->mutableScene();
    const auto& instances = sc->instances();
    if (instanceIndex >= instances.size() || !instances[instanceIndex].trajectory) {
        spdlog::error("[io] export(anim): 实例 {} 无轨迹", instanceIndex);
        return false;
    }
    const uint64_t total = instances[instanceIndex].trajectory->frames.size();
    if (inFrame >= total) {
        spdlog::error("[io] export(anim): 起始帧 {} 超出轨迹帧数 {}", inFrame, total);
        return false;
    }
    // 导出逐帧改写场景坐标；结束后恢复导出前的时间轴帧，否则界面会停在
    // 最后一帧而时间轴仍显示旧帧号（下一次播放才纠正）。
    const uint64_t savedFrame = trajectoryFrame_;
    auto restoreFrame = [this, sc, instanceIndex, savedFrame]() {
        sc->applyTrajectoryFrame(instanceIndex, savedFrame);
        trajectoryFrame_ = savedFrame;
        if (renderer_ != nullptr) {
            renderer_->setTrajectoryFrame(savedFrame);
            renderer_->update(0.0f);
            renderer_->prepareBlocking();
        }
    };
    // 结束帧：0 = 到末帧（UI 默认值就是 0，避免用户必须先知道总帧数）
    const uint64_t last = (outFrame == 0 || outFrame >= total) ? total - 1 : outFrame;
    const uint32_t stepN = step == 0 ? 1 : step;
    if (inFrame > last) {
        spdlog::error("[io] export(anim): 起始帧 {} > 结束帧 {}", inFrame, last);
        restoreFrame();
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        spdlog::error("[io] export(anim): 无法创建输出目录 {}: {}", dirPath, ec.message());
        return false;
    }

    uint32_t written = 0;
    const uint32_t totalFrames = static_cast<uint32_t>((last - inFrame) / stepN + 1);
    for (uint64_t f = inFrame; f <= last; f += stepN) {
        // 帧坐标 → 世界坐标（与播放链路同一实现）→ 命令流重建
        if (!sc->applyTrajectoryFrame(instanceIndex, f)) {
            continue;
        }
        if (source == 2 && orbit) {
            camera_->yaw = savedCamera.yaw +
                           2.0f * 3.14159265358979323846f * static_cast<float>(written) /
                               static_cast<float>(std::max(1u, totalFrames));
        }
        renderer_->update(0.0f);
        renderer_->prepareBlocking();   // 动画逐帧导出：同步确保表面就绪
        char name[64];
        // 顺序命名（0000.png 起）：既便于 ffmpeg 按帧率读取，也不会因为起始帧/步长
        // 不是 0/1 而在序列里留下编号空洞（旧实现用轨迹帧号 f）。
        std::snprintf(name, sizeof(name), "frame_%04u.png", written);
        const std::string out =
            (std::filesystem::path(dirPath) / name).string();
        if (!exportToPng(width, height, out, bgOverride, ssaa, colorDepth, false)) {
            spdlog::error("[io] export(anim): 帧 {} 导出失败，中止", f);
            restoreFrame();
            return false;
        }
        ++written;
    }
    if (frameCountOut != nullptr) {
        *frameCountOut = written;
    }
    spdlog::info("[io] export(anim): {} 帧 -> {} ({}x{}, ssaa={}, {} bit)", written,
                 dirPath, width, height, std::max(1, ssaa), colorDepth);
    restoreFrame();
    return written > 0;
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

bool MolScene::rayTraceSupported() const {
    // 真实 GPU 光追可用性：设备已启用 KHR RT 扩展 + feature，且光追管线可建
    //（惰性初始化一次）。不可用时导出自动回退 CPU（见 exportRayTracedToPng），
    // 「光追」勾选仅用于选择默认后端，并不影响 CPU 回退的可用性。
    return renderer_ != nullptr && renderer_->gpuRayTraceAvailable();
}

void MolScene::setRenderScale(float s) {
    const float v = std::clamp(s, 0.35f, 2.0f);
    if (std::abs(v - renderScale_) < 1e-4f) {
        return;
    }
    renderScale_ = v;
    dirty_ = true;   // 触发 FBO resize
}

void MolScene::setMeshQuality(int detail, int segments) {
    const int d = std::clamp(detail, 1, 4);
    const int seg = std::clamp(segments, 8, 64);
    if (d == meshDetail_ && seg == meshSegments_) {
        return;
    }
    meshDetail_ = d;
    meshSegments_ = seg;
    if (offscreen_) {
        offscreen_->setMeshQuality(d, seg);
    }
    markDirty();
}

} // namespace moleculens::app
