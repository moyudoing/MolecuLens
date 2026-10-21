// =============================================================================
// Moleculens · app · view/tools/styles/styles_tool.cpp —— 渲染设置实现（场景级）
// 规格来源：主方案 §7.9（sliderWithReset：标签左/滑块中/重置右）
//
// 职责边界（2026-09-26 收敛）：本面板只负责**场景级**设置 —— 视角 / 背景 / 雾 /
// 光照 / 方向光 / 视口渲染方式。**表示级**的一切（样式、配色、原子子集、缩放、
// 材质、QuickSurf 参数、整体系合并表面、表示的增删）全部归「图形表示」面板
// （view/tools/reps/reps_tool.cpp）。
// 此前两处都能改表示 —— 虽共用同一个 RepresentationController（不是两份状态），
// 但同一个参数有两个入口会让人以为改的是两个东西，且旧面板的表示"列表"与
// 新面板的表格在视觉上互相矛盾。故旧入口整体移除。
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/tools/styles/styles_tool.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "app/controller/camera/camera_controller.h"
#include "app/controller/render/render_controller.h"
#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"
#include "engine/camera/camera.h"
#include "entity/scene/scene.h"

namespace moleculens::app {

namespace {

// 滑块 + 右侧 R 重置（主方案 §7.9：sliderWithReset）
bool sliderWithReset(const char* label, float* value, float min, float max,
                     float resetValue) {
    bool changed = false;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
    changed |= ImGui::SliderFloat(("##" + std::string(label)).c_str(), value, min, max);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::C_TEXT_MUTED);
    if (ImGui::SmallButton(("R##" + std::string(label)).c_str())) {
        *value = resetValue;
        changed = true;
    }
    ImGui::PopStyleColor();
    return changed;
}

} // namespace

void StylesTool::render(DataManager* dm, MolScene* scene) {

    (void)dm;
    if (!scene || !scene->getCamera()) {
        ImGui::TextDisabled(tr("视口/控制器未就绪"));
        return;
    }
    const auto* sc = scene->scene();

    ImGui::TextDisabled(tr("本面板：场景级设置（视角/背景/雾/光照）。"));
    ImGui::TextDisabled(tr("表示的增删与样式/配色/材质请用「图形表示」面板。"));

    // 视口渲染方式（一.7）：光栅化 / 自研 CPU 光线追踪预览
    {
        UITheme::sectionHeader(tr("视口"));
        bool rtPreview = scene->rayTracePreview();
        if (ImGui::Checkbox(tr("视口光线追踪预览"), &rtPreview)) {
            scene->setRayTracePreview(rtPreview);
        }
        UITheme::helpMarker(tr("开启后 3D 视口用自研 CPU 光线追踪渲染（低分辨率预览，" "相机/场景变化时重渲；导出面板另有高分辨率 RT 导出）"));
    }

    // 视角：正交/透视 + FOV（规格 §7.9 —— 此前是被注释掉的死代码，且背后的
    // CameraController::setPerspective/setFov 也一直无人调用；现经相机控制器接通，
    // 与视口鼠标交互走同一入口）
    UITheme::sectionHeader(tr("视角"));
    if (cameraCtl_ && scene && scene->getCamera()) {
        engine::OrbitCamera* cam = scene->getCamera();
        bool ortho = cam->ortho;
        if (ImGui::Checkbox(tr("正交投影"), &ortho)) {
            cameraCtl_->setPerspective(ortho);
        }
        UITheme::helpMarker(tr("正交投影无近大远小，适合比对结构差异；透视更接近肉眼观感"));

        float fov = cam->fov;
        if (sliderWithReset("FOV (°)", &fov, 10.0f, 120.0f, 45.0f)) {
            cameraCtl_->setFov(fov);
        }
        UITheme::helpMarker(tr("垂直视场角；正交投影下不参与投影，仅影响切回透视后的取景"));

        if (ImGui::Button(tr("复位视角"))) {
            cameraCtl_->reset();   // 清 yaw/pitch/roll + 重新自动取景
        }
        UITheme::helpMarker(tr("恢复默认朝向并按当前场景包围球重新取景"));
    } else {
        ImGui::TextDisabled(tr("相机未就绪（先打开文件）"));
    }

    // 背景（RenderController 写入场景属性 → DIRTY_PROPERTIES → 视口重渲染）
    UITheme::sectionHeader(tr("背景"));
    if (sc) {
        float bg[3] = {sc->properties().bgColor[0], sc->properties().bgColor[1],
                       sc->properties().bgColor[2]};
        if (ImGui::ColorEdit3(tr("背景色"), bg) && renderCtl_) {
            renderCtl_->setBackgroundColor(bg[0], bg[1], bg[2]);
        }
    } else {
        ImGui::TextDisabled(tr("场景未就绪"));
    }

    // 雾效
    UITheme::sectionHeader(tr("雾效"));
    static const char* fogNames[] = {tr("关"), tr("线性"), tr("指数"), tr("指数²")};
    bool fogChanged = false;
    float fogNear = 0.0f, fogFar = 100.0f, fogDensity = 0.05f;
    int fogMode = 0;
    if (sc) {
        fogNear = sc->properties().fog[0];
        fogFar = sc->properties().fog[1];
        fogDensity = sc->properties().fogDensity;
        fogMode = static_cast<int>(sc->properties().fogMode);
    }
    fogChanged |= ImGui::Combo(tr("雾效模式"), &fogMode, fogNames, IM_ARRAYSIZE(fogNames));
    fogChanged |= sliderWithReset(tr("雾近"), &fogNear, 0.0f, 200.0f, 0.0f);
    fogChanged |= sliderWithReset(tr("雾远"), &fogFar, 1.0f, 1000.0f, 100.0f);
    fogChanged |= sliderWithReset(tr("雾密度"), &fogDensity, 0.001f, 0.5f, 0.05f);
    if (fogChanged && renderCtl_) {
        renderCtl_->setFog(fogNear, fogFar, fogMode, fogDensity);
    }

    // 光照（全局光方案，无方向光/点光源）：天顶/地面半球色 + 均匀环境光 → 场景属性
    // → DIRTY_PROPERTIES → 视口重渲染（更新机制：UBO 每帧从 RenderView 重建）
    UITheme::sectionHeader(tr("光照"));
    if (sc && renderCtl_) {
        float sky[3] = {sc->properties().skyColor[0], sc->properties().skyColor[1],
                        sc->properties().skyColor[2]};
        if (ImGui::ColorEdit3(tr("天顶色"), sky)) {
            renderCtl_->setSkyColor(sky[0], sky[1], sky[2]);
        }
        float ground[3] = {sc->properties().groundColor[0], sc->properties().groundColor[1],
                           sc->properties().groundColor[2]};
        if (ImGui::ColorEdit3(tr("地面色"), ground)) {
            renderCtl_->setGroundColor(ground[0], ground[1], ground[2]);
        }
        float acol[3] = {sc->properties().ambientColor[0], sc->properties().ambientColor[1],
                         sc->properties().ambientColor[2]};
        if (ImGui::ColorEdit3(tr("环境光色"), acol)) {
            renderCtl_->setAmbientColor(acol[0], acol[1], acol[2]);
        }
        float ambient = sc->properties().ambient;
        if (sliderWithReset(tr("半球光强度"), &ambient, 0.0f, 1.5f, 0.45f)) {
            renderCtl_->setAmbient(ambient);
        }
        UITheme::helpMarker(tr("半球环境光：天顶亮/地面暗的渐变 + 均匀环境光补光；" "强度越大天顶地面差异越明显"));
    }

    // 方向光（P6 新增，主方案 §6.6.4）：Lambert 漫反射 + Blinn-Phong 高光。
    // 使材质面板的 specular / shininess / metallic / roughness 真实可见
    // （此前这几个材质参数在光栅化路径上完全不参与着色 → 8 套预设零视觉效果）。
    UITheme::sectionHeader(tr("方向光"));
    if (sc && renderCtl_) {
        bool on = sc->properties().dirLightEnabled;
        if (ImGui::Checkbox(tr("启用方向光"), &on)) {
            renderCtl_->setDirLightEnabled(on);
        }
        UITheme::helpMarker(tr("关闭后仅剩半球环境光（无高光/无明暗造型，等同改动前的观感）"));

        ImGui::BeginDisabled(!on);
        // 方位角/仰角（度）：比手填三分量直观；回读真值而非镜像本地变量
        float azDeg = 0.0f, elDeg = 0.0f;
        float az = 0.0f, el = 0.0f;
        renderCtl_->dirLightAngles(&az, &el);
        azDeg = az * 57.29578f;
        elDeg = el * 57.29578f;
        bool anglesChanged = false;
        anglesChanged |= ImGui::SliderFloat(tr("方位角"), &azDeg, -180.0f, 180.0f, "%.0f°");
        anglesChanged |= ImGui::SliderFloat(tr("仰角"), &elDeg, -90.0f, 90.0f, "%.0f°");
        if (anglesChanged) {
            renderCtl_->setDirLightAngles(azDeg / 57.29578f, elDeg / 57.29578f);
        }
        float dl[3] = {sc->properties().dirLightColor[0], sc->properties().dirLightColor[1],
                       sc->properties().dirLightColor[2]};
        if (ImGui::ColorEdit3(tr("光源色"), dl)) {
            renderCtl_->setDirLightColor(dl[0], dl[1], dl[2]);
        }
        float di = sc->properties().dirLightIntensity;
        if (sliderWithReset(tr("强度"), &di, 0.0f, 2.0f, 0.35f)) {
            renderCtl_->setDirLightIntensity(di);
        }

        // 阴影（深度 pass + PCF）。仅球/柱投影 —— TriMesh（表面/卡通/QuickSurf）
        // 不投影，见 ShadowMap 头注释。
        ImGui::Separator();
        bool shOn = sc->properties().shadowsEnabled;
        if (ImGui::Checkbox(tr("投射阴影"), &shOn)) {
            renderCtl_->setShadowsEnabled(shOn);
        }
        UITheme::helpMarker(tr("方向光阴影：光源视角深度图 + 3×3 PCF 软阴影。" "仅球/柱类表示投影（表面/卡通/QuickSurf 不投影）"));
        ImGui::BeginDisabled(!shOn);
        float shStr = sc->properties().shadowStrength;
        if (sliderWithReset(tr("阴影强度"), &shStr, 0.0f, 1.0f, 0.65f)) {
            renderCtl_->setShadowStrength(shStr);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
}

} // namespace moleculens::app
