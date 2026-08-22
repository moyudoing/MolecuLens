// =============================================================================
// MolecuLens · app · view/tools/styles/styles_tool.h —— 渲染设置面板
// 规格来源：主方案 §7.9（样式/着色方案/正交透视/FOV/球体缩放/键半径/
//           背景色/裁剪面/雾效；均带重置）
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class StylesTool : public BaseToolView {
public:
    const char* getTitle() const override { return "渲染设置"; }
    void render(DataManager* dm, MolScene* scene) override;
};

} // namespace moleculens::app
