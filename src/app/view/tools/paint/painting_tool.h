// =============================================================================
// MolecuLens · app · view/tools/paint/painting_tool.h —— 绘图工具面板
// 规格来源：主方案 §7.9（交互模式：选择/测量/标记——首期占位/预留：
//           UI 就绪，拾取逻辑待 §5.6 对接）
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class PaintingTool : public BaseToolView {
public:
    const char* getTitle() const override { return "绘图工具"; }
    void render(DataManager* dm, MolScene* scene) override;
};

} // namespace moleculens::app
