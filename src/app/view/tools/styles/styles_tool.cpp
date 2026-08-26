// =============================================================================
// MolecuLens · app · view/tools/styles/styles_tool.cpp —— 渲染设置实现
// 规格来源：主方案 §7.9（sliderWithReset：标签左/滑块中/重置右）
// 变更：样式/着色/缩放/背景/雾效参数直通 RepresentationController /
//       RenderController（此前为静态局部变量不生效——见 moleculens-fixes）
// =============================================================================

#include "app/view/tools/styles/styles_tool.h"

#include <cmath>

#include "app/controller/rep/representation_controller.h"
#include "app/controller/render/render_controller.h"
#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"
#include "engine/camera/camera.h"
#include "engine/rep/representation.h"
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
    if (!repCtl_ || !scene || !scene->getCamera()) {
        ImGui::TextDisabled("视口/控制器未就绪");
        return;
    }

    // 当前表示（UI 作用目标：第一个表示；读取其真实状态而非静态默认值）
    const auto& reps = repCtl_->representations();
    if (reps.empty()) {
        ImGui::TextDisabled("无表示");
        return;
    }

    constexpr int activeRep = 0;
    const engine::Representation& rep = reps[activeRep];
    const auto* sc = scene->scene();

    // 视口渲染方式（一.7）：光栅化 / 自研 CPU 光线追踪预览
    {
        bool rtPreview = scene->rayTracePreview();
        if (ImGui::Checkbox("视口光线追踪预览", &rtPreview)) {
            scene->setRayTracePreview(rtPreview);
        }
        UITheme::helpMarker("开启后 3D 视口用自研 CPU 光线追踪渲染（低分辨率预览，"
                            "相机/场景变化时重渲；导出面板另有高分辨率 RT 导出）");
        ImGui::Separator();
    }

    // 样式（表示类型，主方案 §7.9：线状/棍状/球棍/CPK/VDW/甘草棍/表面）
    // 名称 → 类型显式映射（此前用下标直接 cast 枚举，名称与类型错位：
    //  "球棍" 实为 Stick、"CPK" 实为 BallAndStick、VDW 不可用）
    static const char* styleNames[] = {"线状", "棍状", "球棍", "CPK", "VDW",
                                       "甘草棍", "表面", "QuickSurf", "丝带",
                                       "管状", "新卡通"};
    static constexpr engine::RepresentationType styleTypes[] = {
        engine::RepresentationType::Wireframe,
        engine::RepresentationType::Stick,
        engine::RepresentationType::BallAndStick,
        engine::RepresentationType::Spacefill,   // CPK：范德华半径空间填充（受球体缩放）
        engine::RepresentationType::Vdw,         // VDW：全范德华半径空间填充（不受缩放）
        engine::RepresentationType::Cartoon,
        engine::RepresentationType::Surface,
        engine::RepresentationType::QuickSurf,   // 高斯密度等值面
        engine::RepresentationType::Ribbon,      // 丝带：平滑 CA 骨架带状
        engine::RepresentationType::Tube,        // 管状：圆管骨架
        engine::RepresentationType::NewCartoon,  // 新卡通：改进的二级结构渲染
    };

    int styleIdx = 2;   // 2 默认球棍
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(styleTypes)); ++i) {
        if (styleTypes[i] == rep.type) {
            styleIdx = i;
            break;
        }
    }

    // 样式选项
    ImGui::Text("样式");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
    if (ImGui::Combo("##style", &styleIdx, styleNames, IM_ARRAYSIZE(styleNames))) {
        repCtl_->setType(activeRep, styleTypes[styleIdx]);
    }
    // UITheme::helpMarker("CPK/VDW = 范德华空间填充；QuickSurf = 高斯密度等值面（可调阈值/网格）");
    ImGui::SameLine();

    // QuickSurf 参数（等值面阈值 / 网格间距；radscale 复用上方"球体缩放"）
    if (rep.type == engine::RepresentationType::QuickSurf) {
        float iso = rep.surfIsovalue;
        if (sliderWithReset("等值面阈值", &iso, 0.05f, 2.0f, 0.5f)) {
            repCtl_->setSurfIsovalue(activeRep, iso);
        }
        float gs = rep.surfGridSpacing;
        if (sliderWithReset("网格间距", &gs, 0.2f, 2.0f, 0.5f)) {
            repCtl_->setSurfGridSpacing(activeRep, gs);
        }
        UITheme::helpMarker("网格间距越小表面越精细（计算越慢）；等值面阈值控制表面贴合度");
    }
    // 体系级表面：Surface/QuickSurf 整体系（含溶剂分子）一个整体表面——
    // 溶剂化大体系（md.gro 大量 SOL）每分子一个独立小表面既慢又碎，
    // 开启后按整个体系聚合为一个连续表面（渲染器 drawSystemSurface/drawSystemQuickSurf）
    if (rep.type == engine::RepresentationType::Surface ||
        rep.type == engine::RepresentationType::QuickSurf) {
        bool ss = rep.systemSurface;
        if (ImGui::Checkbox("整体系合并表面", &ss)) {
            repCtl_->setSystemSurface(activeRep, ss);
        }
        UITheme::helpMarker("开启后按整个体系生成一个整体表面（含溶剂分子），"
                            "而非每个分子各自一个独立小表面；溶剂化大体系（如 md.gro）用");
        ImGui::Separator();
    }

    // 着色方案
    static const char* colorNames[] = {"元素 CPK", "按链", "按残基", "按 BFactor",
                                       "统一色"};
    int colorIdx = static_cast<int>(rep.colorScheme);
    ImGui::Text("着色");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::Combo("##color", &colorIdx, colorNames, IM_ARRAYSIZE(colorNames))) {
        repCtl_->setColorScheme(activeRep, static_cast<engine::ColorScheme>(colorIdx));
    }

    // // 正交/透视 + FOV（读取相机当前状态，写入即生效）
    // engine::OrbitCamera* cam = scene->getCamera();
    // bool ortho = cam->ortho;
    // if (ImGui::Checkbox("正交投影", &ortho)) {
    //     cam->ortho = ortho;
    // }
    // float fov = cam->fov;
    // if (sliderWithReset("FOV (°)", &fov, 10.0f, 120.0f, 45.0f)) {
    //     cam->setFov(fov);
    // }

    // 球体缩放 / 键半径（表示控制器）
    float sphereScale = rep.radiusScale;
    if (sliderWithReset("球体缩放", &sphereScale, 0.1f, 3.0f, 1.0f)) {
        repCtl_->setRadiusScale(activeRep, sphereScale);
    }
    float bondRadius = rep.bondScale;
    if (sliderWithReset("键半径", &bondRadius, 0.2f, 3.0f, 1.0f)) {
        repCtl_->setBondScale(activeRep, bondRadius);
    }

    // 背景（RenderController 写入场景属性 → DIRTY_PROPERTIES → 视口重渲染）
    UITheme::sectionHeader("背景");
    if (sc) {
        float bg[3] = {sc->properties().bgColor[0], sc->properties().bgColor[1],
                       sc->properties().bgColor[2]};
        if (ImGui::ColorEdit3("背景色", bg) && renderCtl_) {
            renderCtl_->setBackgroundColor(bg[0], bg[1], bg[2]);
        }
    } else {
        ImGui::TextDisabled("场景未就绪");
    }

    // 雾效
    UITheme::sectionHeader("雾效");
    static const char* fogNames[] = {"关", "线性", "指数", "指数²"};
    bool fogChanged = false;
    float fogNear = 0.0f, fogFar = 100.0f, fogDensity = 0.05f;
    int fogMode = 0;
    if (sc) {
        fogNear = sc->properties().fog[0];
        fogFar = sc->properties().fog[1];
        fogDensity = sc->properties().fogDensity;
        fogMode = static_cast<int>(sc->properties().fogMode);
    }
    fogChanged |= ImGui::Combo("雾效模式", &fogMode, fogNames, IM_ARRAYSIZE(fogNames));
    fogChanged |= sliderWithReset("雾近", &fogNear, 0.0f, 200.0f, 0.0f);
    fogChanged |= sliderWithReset("雾远", &fogFar, 1.0f, 1000.0f, 100.0f);
    fogChanged |= sliderWithReset("雾密度", &fogDensity, 0.001f, 0.5f, 0.05f);
    if (fogChanged && renderCtl_) {
        renderCtl_->setFog(fogNear, fogFar, fogMode, fogDensity);
    }

    // 光照（全局光方案，无方向光/点光源）：天顶/地面半球色 + 均匀环境光 → 场景属性
    // → DIRTY_PROPERTIES → 视口重渲染（更新机制：UBO 每帧从 RenderView 重建）
    UITheme::sectionHeader("光照");
    if (sc && renderCtl_) {
        float sky[3] = {sc->properties().skyColor[0], sc->properties().skyColor[1],
                        sc->properties().skyColor[2]};
        if (ImGui::ColorEdit3("天顶色", sky)) {
            renderCtl_->setSkyColor(sky[0], sky[1], sky[2]);
        }
        float ground[3] = {sc->properties().groundColor[0], sc->properties().groundColor[1],
                           sc->properties().groundColor[2]};
        if (ImGui::ColorEdit3("地面色", ground)) {
            renderCtl_->setGroundColor(ground[0], ground[1], ground[2]);
        }
        float acol[3] = {sc->properties().ambientColor[0], sc->properties().ambientColor[1],
                         sc->properties().ambientColor[2]};
        if (ImGui::ColorEdit3("环境光色", acol)) {
            renderCtl_->setAmbientColor(acol[0], acol[1], acol[2]);
        }
        float ambient = sc->properties().ambient;
        if (sliderWithReset("半球光强度", &ambient, 0.0f, 1.5f, 0.45f)) {
            renderCtl_->setAmbient(ambient);
        }
        UITheme::helpMarker("全局光照（无方向光/点光源）：天顶亮/地面暗的半球环境光 + "
                            "均匀环境光补光；强度越大天顶地面差异越明显");
    }
}

} // namespace moleculens::app
