// =============================================================================
// MolecuLens · app · view/tools/styles/styles_tool.cpp —— 渲染设置实现
// 规格来源：主方案 §7.9（sliderWithReset：标签左/滑块中/重置右）
// =============================================================================

#include "app/view/tools/styles/styles_tool.h"

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
    if (!scene || !scene->getCamera()) {
        ImGui::TextDisabled("视口未就绪");
        return;
    }
    // 样式（表示类型，主方案 §7.9：线状/球棍/CPK/VDW/甘草棍/表面/快速表面）
    static int styleIdx = 1;
    static const char* styleNames[] = {"线状", "球棍", "CPK", "VDW",
                                       "甘草棍", "表面", "快速表面"};
    ImGui::Text("样式");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 30.0f);
    ImGui::Combo("##style", &styleIdx, styleNames, IM_ARRAYSIZE(styleNames));
    ImGui::SameLine();
    UITheme::helpMarker("值越大原子越饱满");

    // 着色方案
    static int colorIdx = 0;
    static const char* colorNames[] = {"元素 CPK", "按链", "按残基", "按 BFactor",
                                       "统一色"};
    ImGui::Text("着色");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::Combo("##color", &colorIdx, colorNames, IM_ARRAYSIZE(colorNames));

    // 正交/透视 + FOV
    engine::OrbitCamera* cam = scene->getCamera();
    static bool ortho = false;
    if (ImGui::Checkbox("正交投影", &ortho)) {
        cam->ortho = ortho;
    }
    static float fov = 45.0f;
    if (sliderWithReset("FOV (°)", &fov, 10.0f, 120.0f, 45.0f)) {
        cam->setFov(fov);
    }

    // 球体缩放 / 键半径（M2 接入表示控制器）
    static float sphereScale = 1.0f;
    sliderWithReset("球体缩放", &sphereScale, 0.1f, 3.0f, 1.0f);
    static float bondRadius = 0.15f;
    sliderWithReset("键半径", &bondRadius, 0.02f, 0.5f, 0.15f);

    UITheme::sectionHeader("背景");
    static float bg[3] = {0.1f, 0.1f, 0.15f};
    if (ImGui::ColorEdit3("背景色", bg)) {
        // M2：RenderController::setBackgroundColor
    }
    UITheme::sectionHeader("雾效");
    static int fogMode = 0;
    static const char* fogNames[] = {"关", "线性", "指数", "指数²"};
    ImGui::Combo("雾效模式", &fogMode, fogNames, IM_ARRAYSIZE(fogNames));
    static float fogNear = 0.0f, fogFar = 100.0f, fogDensity = 0.05f;
    sliderWithReset("雾近", &fogNear, 0.0f, 200.0f, 0.0f);
    sliderWithReset("雾远", &fogFar, 1.0f, 1000.0f, 100.0f);
    sliderWithReset("雾密度", &fogDensity, 0.001f, 0.5f, 0.05f);
}

} // namespace moleculens::app
