// =============================================================================
// MolecuLens · app · view/tools/atom_rep/atom_rep_tool.cpp —— 原子级渲染控制实现
// =============================================================================

#include "app/view/tools/atom_rep/atom_rep_tool.h"

#include <imgui.h>

#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"

namespace moleculens::app {

void AtomRepTool::render(DataManager* dm, MolScene* scene) {
    if (!dm || !scene) {
        ImGui::TextDisabled("数据未就绪");
        return;
    }

    ImGui::Text("原子级渲染控制");
    ImGui::Separator();

    // 原子选择区域
    renderAtomSelection();

    ImGui::Separator();

    // 原子组列表
    renderGroupList();

    ImGui::Separator();

    // 组属性编辑
    if (selectedGroup_ >= 0) {
        renderGroupProperties();
    }
}

void AtomRepTool::renderAtomSelection() {
    ImGui::Text("原子选择");

    // 输入原子索引
    static char atomInput[256] = "";
    ImGui::InputText("原子索引", atomInput, sizeof(atomInput));
    ImGui::SameLine();
    UITheme::helpMarker("输入原子索引（逗号分隔），如：0,1,2,5-10");

    if (ImGui::Button("添加到选择")) {
        // 解析输入（简化版，仅支持逗号分隔和范围）
        selectedAtoms_.clear();
        std::string input(atomInput);
        size_t pos = 0;

        while (pos < input.size()) {
            // 跳过逗号和空格
            while (pos < input.size() && (input[pos] == ',' || input[pos] == ' ')) {
                pos++;
            }
            if (pos >= input.size()) break;

            // 检查是否有范围（-）
            size_t dashPos = input.find('-', pos);
            size_t commaPos = input.find(',', pos);

            if (dashPos != std::string::npos &&
                (commaPos == std::string::npos || dashPos < commaPos)) {
                // 范围：5-10
                uint32_t start = std::stoul(input.substr(pos, dashPos - pos));
                pos = dashPos + 1;
                size_t endPos = (commaPos != std::string::npos) ? commaPos : input.size();
                uint32_t end = std::stoul(input.substr(pos, endPos - pos));
                pos = endPos;

                for (uint32_t i = start; i <= end; ++i) {
                    selectedAtoms_.push_back(i);
                }
            } else {
                // 单个值
                size_t endPos = (commaPos != std::string::npos) ? commaPos : input.size();
                uint32_t val = std::stoul(input.substr(pos, endPos - pos));
                selectedAtoms_.push_back(val);
                pos = endPos;
            }
        }

        // 去重和排序
        std::sort(selectedAtoms_.begin(), selectedAtoms_.end());
        selectedAtoms_.erase(std::unique(selectedAtoms_.begin(), selectedAtoms_.end()),
                            selectedAtoms_.end());
    }

    // 显示当前选择的原子
    if (!selectedAtoms_.empty()) {
        ImGui::Text("已选择 %zu 个原子:", selectedAtoms_.size());
        ImGui::BeginChild("##selected_atoms", ImVec2(-1, 100), true);

        std::string atomList;
        for (uint32_t idx : selectedAtoms_) {
            atomList += std::to_string(idx) + " ";
        }
        ImGui::TextWrapped("%s", atomList.c_str());
        ImGui::EndChild();
    }
}

void AtomRepTool::renderGroupList() {
    ImGui::Text("原子组列表");

    // 创建新组
    ImGui::InputText("组名称", groupName_, sizeof(groupName_));
    ImGui::SameLine();

    static int newGroupType = 0;
    const char* typeNames[] = {"线框", "棍状", "球棍", "空间填充", "卡通", "表面",
                               "标签", "VDW", "QuickSurf", "甘草棍", "管状", "新卡通", "丝带"};
    ImGui::Combo("##new_type", &newGroupType, typeNames, 13);

    if (ImGui::Button("创建原子组")) {
        if (!selectedAtoms_.empty()) {
            auto type = static_cast<engine::RepresentationType>(newGroupType);
            int32_t id = manager_.addGroup(groupName_, selectedAtoms_, type,
                                           engine::ColorScheme::ElementCPK);
            if (id >= 0) {
                selectedGroup_ = id;
                // 清空选择
                selectedAtoms_.clear();
                groupName_[0] = '\0';
            }
        }
    }

    ImGui::Separator();

    // 显示所有组
    const auto& groups = manager_.groups();
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        ImGui::PushID(i);

        bool visible = groups[i].visible;
        if (ImGui::Checkbox("##vis", &visible)) {
            manager_.updateGroupVisibility(i, visible);
        }
        ImGui::SameLine();

        bool selected = (selectedGroup_ == i);
        if (ImGui::Selectable(groups[i].name.c_str(), selected)) {
            selectedGroup_ = i;
        }

        ImGui::SameLine();
        ImGui::Text("(%zu atoms)", groups[i].atomIndices.size());

        ImGui::PopID();
    }
}

void AtomRepTool::renderGroupProperties() {
    const auto& groups = manager_.groups();
    if (selectedGroup_ < 0 || selectedGroup_ >= static_cast<int>(groups.size())) {
        return;
    }

    ImGui::Text("组属性: %s", groups[selectedGroup_].name.c_str());
    ImGui::Separator();

    // 渲染类型
    int typeIdx = static_cast<int>(groups[selectedGroup_].type);
    const char* typeNames[] = {"线框", "棍状", "球棍", "空间填充", "卡通", "表面",
                               "标签", "VDW", "QuickSurf", "甘草棍", "管状", "新卡通", "丝带"};
    if (ImGui::Combo("渲染类型", &typeIdx, typeNames, 13)) {
        manager_.updateGroupType(selectedGroup_,
                                static_cast<engine::RepresentationType>(typeIdx));
    }

    // 配色方案
    int colorIdx = static_cast<int>(groups[selectedGroup_].colorScheme);
    const char* colorNames[] = {"元素CPK", "按链", "按残基", "按BFactor", "统一"};
    if (ImGui::Combo("配色方案", &colorIdx, colorNames, 5)) {
        manager_.updateGroupColorScheme(selectedGroup_,
                                       static_cast<engine::ColorScheme>(colorIdx));
    }

    // 半径缩放
    float radiusScale = groups[selectedGroup_].radiusScale;
    if (ImGui::SliderFloat("半径缩放", &radiusScale, 0.1f, 3.0f, "%.2f")) {
        manager_.updateGroupRadiusScale(selectedGroup_, radiusScale);
    }

    // 删除组
    if (ImGui::Button("删除组")) {
        manager_.removeGroup(selectedGroup_);
        selectedGroup_ = -1;
    }
}

} // namespace moleculens::app
