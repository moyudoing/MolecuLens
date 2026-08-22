// =============================================================================
// MolecuLens · app · view/tools/mol_list/mol_list_tool.h —— 分子列表面板
// 规格来源：主方案 §7.9（列出所有分子，点击设当前、X 删除；当前高亮并
//           缩进显示原子/残基/帧/表示数）
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class MolListTool : public BaseToolView {
public:
    const char* getTitle() const override { return "分子列表"; }
    void render(DataManager* dm, MolScene* scene) override;
};

} // namespace moleculens::app
