// =============================================================================
// MolecuLens · app · view/tools/mol_list/mol_list_tool.cpp —— 分子列表实现
// 规格来源：主方案 §7.9（当前高亮 + 原子/残基/帧/表示数缩进）
// =============================================================================

#include "app/view/tools/mol_list/mol_list_tool.h"

#include <cstdio>

#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"

namespace moleculens::app {

void MolListTool::render(DataManager* dm, MolScene* scene) {
    (void)scene;
    if (!dm) {
        return;
    }
    const size_t count = dm->moleculeCount();
    if (count == 0) {
        ImGui::TextDisabled("暂无分子（Ctrl+O 打开 PDB/GRO）");
        return;
    }
    // 当前分子（M1：固定选中第一个；M2 接入选中态）
    static size_t selected = 0;
    if (selected >= count) {
        selected = 0;
    }
    for (size_t i = 0; i < count; ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), "molecule_%zu", i);
        const bool isCurrent = (i == selected);
        if (isCurrent) {
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z,
                                         0.25f));
        }
        if (ImGui::Selectable(label, isCurrent)) {
            selected = i;
        }
        if (isCurrent) {
            ImGui::PopStyleColor();
            // 缩进显示统计（主方案 §7.9）
            ImGui::Indent();
            char info[128];
            std::snprintf(info, sizeof(info), "原子 %zu · 键 %zu · 帧 %zu",
                          dm->atomCount(i), dm->bondCount(i), dm->frameCount(i));
            ImGui::TextDisabled("%s", info);
            ImGui::Unindent();
        }
        if (ImGui::BeginPopupContextItem(label)) {
            if (ImGui::MenuItem("删除")) {
                // M2：SceneController.removeInstance
            }
            ImGui::EndPopup();
        }
    }
}

} // namespace moleculens::app
