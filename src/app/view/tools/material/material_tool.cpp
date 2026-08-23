// =============================================================================
// MolecuLens · app · view/tools/material/material_tool.cpp —— 材质面板实现
// 规格来源：主方案 §6.6.2（8 预设数值表）/ §6.6.6（调整 → 表示材质 →
//           DIRTY_REPRESENTATION）
// =============================================================================

#include "app/view/tools/material/material_tool.h"

#include <cmath>

#include "app/controller/rep/representation_controller.h"
#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"

namespace moleculens::app {

const MaterialTool::Preset* MaterialTool::presets() {
    // 主方案 §6.6.2 预设表（数值以视觉调校为准）
    static const Preset kPresets[] = {
        {"默认", {0.60f, 0.60f, 0.60f, 1}, {0.90f, 0.90f, 0.90f, 1},
         {0.30f, 0.30f, 0.30f, 1}, 32, 0.00f, 0.40f, 1.00f},
        {"金属", {0.30f, 0.30f, 0.30f, 1}, {0.70f, 0.70f, 0.70f, 1},
         {1.00f, 1.00f, 1.00f, 1}, 128, 1.00f, 0.25f, 1.00f},
        {"光泽", {0.50f, 0.50f, 0.50f, 1}, {0.90f, 0.90f, 0.90f, 1},
         {0.80f, 0.80f, 0.80f, 1}, 96, 0.00f, 0.15f, 1.00f},
        {"哑光", {0.80f, 0.80f, 0.80f, 1}, {0.95f, 0.95f, 0.95f, 1},
         {0.10f, 0.10f, 0.10f, 1}, 8, 0.00f, 0.85f, 1.00f},
        {"半透明", {0.50f, 0.50f, 0.50f, 1}, {0.80f, 0.80f, 0.80f, 1},
         {0.40f, 0.40f, 0.40f, 1}, 32, 0.00f, 0.40f, 0.60f},
        {"玻璃", {0.30f, 0.30f, 0.30f, 1}, {0.70f, 0.70f, 0.70f, 1},
         {0.90f, 0.90f, 0.90f, 1}, 128, 0.00f, 0.05f, 0.25f},
        {"粗糙金属", {0.30f, 0.30f, 0.30f, 1}, {0.70f, 0.70f, 0.70f, 1},
         {0.70f, 0.70f, 0.70f, 1}, 64, 1.00f, 0.70f, 1.00f},
        {"塑料", {0.40f, 0.40f, 0.40f, 1}, {0.85f, 0.85f, 0.85f, 1},
         {0.60f, 0.60f, 0.60f, 1}, 64, 0.00f, 0.30f, 1.00f},
    };
    return kPresets;
}

int MaterialTool::detectPreset(const Preset& cur, float tolerance) {
    const Preset* list = presets();
    for (int i = 0; i < 8; ++i) {
        const Preset& p = list[i];
        if (std::fabs(p.shininess - cur.shininess) > tolerance ||
            std::fabs(p.metallic - cur.metallic) > tolerance ||
            std::fabs(p.roughness - cur.roughness) > tolerance ||
            std::fabs(p.opacity - cur.opacity) > tolerance) {
            continue;
        }
        bool match = true;
        for (int c = 0; c < 3; ++c) {
            if (std::fabs(p.ambient[c] - cur.ambient[c]) > tolerance ||
                std::fabs(p.diffuse[c] - cur.diffuse[c]) > tolerance ||
                std::fabs(p.specular[c] - cur.specular[c]) > tolerance) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return -1;   // 自定义
}

void MaterialTool::presetParams(int index, float* ambient, float* diffuse,
                                float* specular, float* shininess, float* metallic,
                                float* roughness, float* opacity) {
    const Preset& p = presets()[index];
    for (int i = 0; i < 4; ++i) {
        ambient[i] = p.ambient[i];
        diffuse[i] = p.diffuse[i];
        specular[i] = p.specular[i];
    }
    *shininess = p.shininess;
    *metallic = p.metallic;
    *roughness = p.roughness;
    *opacity = p.opacity;
}

void MaterialTool::render(DataManager* dm, MolScene* scene) {
    (void)dm;
    (void)scene;
    if (!repCtl_) {
        ImGui::TextDisabled("控制器未就绪");
        return;
    }
    // 读取当前表示的真实材质（而非静态默认值）
    const auto& reps = repCtl_->representations();
    if (reps.empty()) {
        ImGui::TextDisabled("无表示");
        return;
    }
    constexpr int activeRep = 0;
    const engine::Representation& rep = reps[activeRep];
    float ambient[4], diffuse[4], specular[4];
    for (int i = 0; i < 4; ++i) {
        ambient[i] = rep.ambient[i];
        diffuse[i] = rep.diffuse[i];
        specular[i] = rep.specular[i];
    }
    float shininess = rep.shininess, metallic = rep.metallic;
    float roughness = rep.roughness, opacity = rep.opacity;

    // 预设选择（detectPreset 容差回显；手动改参自动回"自定义"）
    Preset cur;
    for (int i = 0; i < 4; ++i) {
        cur.ambient[i] = ambient[i];
        cur.diffuse[i] = diffuse[i];
        cur.specular[i] = specular[i];
    }
    cur.shininess = shininess;
    cur.metallic = metallic;
    cur.roughness = roughness;
    cur.opacity = opacity;
    int detected = detectPreset(cur, 0.03f);

    static const char* presetNames[] = {"默认", "金属", "光泽", "哑光",
                                        "半透明", "玻璃", "粗糙金属", "塑料", "自定义"};
    int comboIdx = detected >= 0 ? detected : 8;
    ImGui::Text("材质预设");
    ImGui::SameLine();
    bool materialChanged = false;
    if (ImGui::Combo("##preset", &comboIdx, presetNames, IM_ARRAYSIZE(presetNames))) {
        if (comboIdx < 8) {
            presetParams(comboIdx, ambient, diffuse, specular, &shininess, &metallic,
                         &roughness, &opacity);
            materialChanged = true;
        }
    }
    UITheme::helpMarker("8 套预设：默认/金属/光泽/哑光/半透明/玻璃/粗糙金属/塑料");

    UITheme::sectionHeader("Phong 参数");
    materialChanged |= ImGui::ColorEdit3("环境色", ambient);
    materialChanged |= ImGui::ColorEdit3("漫反射色", diffuse);
    materialChanged |= ImGui::ColorEdit3("高光色", specular);
    materialChanged |= ImGui::SliderFloat("光泽度", &shininess, 1.0f, 256.0f, "%.0f");
    UITheme::sectionHeader("PBR 扩展");
    materialChanged |= ImGui::SliderFloat("金属度", &metallic, 0.0f, 1.0f);
    materialChanged |= ImGui::SliderFloat("粗糙度", &roughness, 0.0f, 1.0f);
    materialChanged |= ImGui::SliderFloat("不透明度", &opacity, 0.05f, 1.0f);
    // 变更即应用 → RepresentationController::setMaterial（DIRTY_REPRESENTATION）
    if (materialChanged) {
        repCtl_->setMaterial(activeRep, ambient, diffuse, specular, shininess,
                             metallic, roughness, opacity);
    }
}

} // namespace moleculens::app
