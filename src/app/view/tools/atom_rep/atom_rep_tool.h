// =============================================================================
// MolecuLens · app · view/tools/atom_rep/atom_rep_tool.h —— 原子级渲染控制工具
// 功能：支持单个原子或原子组的独立渲染模式切换
// =============================================================================

#pragma once

#include <string>
#include <vector>

#include "app/view/tool/base_tool_view.h"
#include "engine/rep/atom/atom_representation.h"

namespace moleculens::app {

class AtomRepTool : public BaseToolView {
public:
    AtomRepTool() : BaseToolView("原子级控制", "AtomRep") {}
    ~AtomRepTool() override = default;

    void render(DataManager* dm, MolScene* scene) override;

    // 获取原子表示管理器（供渲染器使用）
    engine::AtomRepresentationManager& getManager() { return manager_; }

private:
    engine::AtomRepresentationManager manager_;

    // UI 状态
    char groupName_[128] = "新原子组";
    std::vector<uint32_t> selectedAtoms_;
    int selectedGroup_ = -1;

    void renderAtomSelection();
    void renderGroupList();
    void renderGroupProperties();
};

} // namespace moleculens::app
