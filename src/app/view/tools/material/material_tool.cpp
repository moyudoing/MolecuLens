// =============================================================================
// Moleculens · app · view/tools/material/material_tool.cpp —— 材质面板实现
// 规格来源：主方案 §6.6.2（材质预设）/ §6.6.6（调整 → 表示材质 → 脏位置位）
// 材质库：engine/render/render_parameter.h 的 kMaterialPresets（23 套，与 VMD
//         Material 下拉同名）；本面板只做选择与逐参数微调，不另存一份数值表。
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/tools/material/material_tool.h"

#include <cmath>

#include "app/controller/rep/representation_controller.h"
#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"
#include "engine/render/render_parameter.h"

namespace moleculens::app {

namespace {

// 逐项容差比较当前参数与某预设是否一致。
// 用途仅剩"用户手改参数后把下拉切回自定义"——**不再用它反查预设名**：
// 材质库里有 Glass1/2/3、Edgy/EdgyShiny 这类数值接近的项，容差反查会互相误判。
// 选中项由 Representation::materialPreset 显式记录。
bool paramsMatch(const engine::Representation& rep, const engine::MaterialPreset& p,
                 float tolerance) {
    if (std::fabs(rep.shininess - p.shininess) > tolerance ||
        std::fabs(rep.metallic - p.metallic) > tolerance ||
        std::fabs(rep.roughness - p.roughness) > tolerance ||
        std::fabs(rep.opacity - p.opacity) > tolerance) {
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        if (std::fabs(rep.ambient[c] - p.ambient[c]) > tolerance ||
            std::fabs(rep.diffuse[c] - p.diffuse[c]) > tolerance ||
            std::fabs(rep.specular[c] - p.specular[c]) > tolerance) {
            return false;
        }
    }
    return true;
}

} // namespace

void MaterialTool::render(DataManager* dm, MolScene* scene) {
    (void)dm;
    (void)scene;
    if (!repCtl_) {
        ImGui::TextDisabled(tr("控制器未就绪"));
        return;
    }
    // 编辑目标与"渲染设置"面板共用同一下标（表示控制器持有），否则两个面板会
    // 各改各的表示而看不出来。
    const auto& reps = repCtl_->representations();
    if (reps.empty()) {
        ImGui::TextDisabled(tr("无表示"));
        return;
    }
    const int32_t activeRep = repCtl_->activeIndex();
    const engine::Representation& rep = reps[static_cast<size_t>(activeRep)];
    ImGui::TextDisabled(tr("正在编辑：%s"), rep.name.c_str());

    // —— 材质预设（23 套，名字与 VMD 的 Material 下拉对齐）——
    // 回显以 rep.materialPreset 为准；若该下标对应的数值与当前参数已不符
    // （外部改了参数 / 载入了别的场景），显示"自定义"。
    int comboIdx = rep.materialPreset;
    if (comboIdx < 0 || comboIdx >= engine::kMaterialPresetCount ||
        !paramsMatch(rep, engine::materialPresetAt(comboIdx), 0.01f)) {
        comboIdx = engine::kMaterialPresetCount;   // tr("自定义")项的槽位
    }
    const char* presetNames[engine::kMaterialPresetCount + 1];
    for (int i = 0; i < engine::kMaterialPresetCount; ++i) {
        presetNames[i] = engine::materialPresetAt(i).name;
    }
    presetNames[engine::kMaterialPresetCount] = tr("自定义");

    ImGui::Text(tr("材质预设"));
    ImGui::SameLine();
    if (ImGui::Combo("##material", &comboIdx, presetNames,
                     engine::kMaterialPresetCount + 1)) {
        if (comboIdx < engine::kMaterialPresetCount) {
            const engine::MaterialPreset& p = engine::materialPresetAt(comboIdx);
            float ambient[4] = {p.ambient[0], p.ambient[1], p.ambient[2], 1.0f};
            float diffuse[4] = {p.diffuse[0], p.diffuse[1], p.diffuse[2], 1.0f};
            float specular[4] = {p.specular[0], p.specular[1], p.specular[2], 1.0f};
            repCtl_->setMaterialPreset(activeRep, comboIdx, ambient, diffuse, specular,
                                       p.shininess, p.metallic, p.roughness, p.opacity);
        }
    }
    UITheme::helpMarker(tr("23 套材质（名字对齐 VMD 的 Material 下拉）。\n" "数值是按本引擎 Phong/PBR 管线观感调校的近似值，非 VMD 原表；\n" "Edgy*/AO* 在 VMD 里依赖描边与环境光遮蔽通道，本引擎以" "高光/环境光近似。"));

    // —— 逐参数微调（改任一项即转"自定义"）——
    float ambient[4], diffuse[4], specular[4];
    for (int i = 0; i < 4; ++i) {
        ambient[i] = rep.ambient[i];
        diffuse[i] = rep.diffuse[i];
        specular[i] = rep.specular[i];
    }
    float shininess = rep.shininess, metallic = rep.metallic;
    float roughness = rep.roughness, opacity = rep.opacity;

    bool changed = false;
    UITheme::sectionHeader(tr("Phong 参数"));
    changed |= ImGui::ColorEdit3(tr("环境色"), ambient);
    changed |= ImGui::ColorEdit3(tr("漫反射色"), diffuse);
    changed |= ImGui::ColorEdit3(tr("高光色"), specular);
    changed |= ImGui::SliderFloat(tr("光泽度"), &shininess, 1.0f, 256.0f, "%.0f");
    UITheme::helpMarker(tr("光泽度 = 高光锐度（Phong 指数）。越小越哑光，越大高光越集中；" "需配合高光色才明显。"));
    UITheme::sectionHeader(tr("PBR 扩展"));
    changed |= ImGui::SliderFloat(tr("金属度"), &metallic, 0.0f, 1.0f);
    UITheme::sectionHeader(tr("透明度"));
    // 透明度（alpha/opacity）：< 1 时几何进入透明通道（混合管线按不透明度排序绘制）
    changed |= ImGui::SliderFloat(tr("不透明度"), &opacity, 0.05f, 1.0f);
    UITheme::helpMarker(tr("1 = 完全不透明；越小越透。与体系渲染模式「透明」叠加时取更小者。"));

    if (changed) {
        // 手改参数 → 不再是任何预设（materialPreset = -1 = 自定义）
        repCtl_->setMaterial(activeRep, -1, ambient, diffuse, specular, shininess,
                             metallic, roughness, opacity);
    }
}

} // namespace moleculens::app
