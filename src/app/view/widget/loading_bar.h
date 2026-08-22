// =============================================================================
// MolecuLens · app · view/widget/loading_bar.h —— 加载条组件（确定/不确定双模式）
// 规格来源：主方案 §7.5（progress >= 0 → 标准 ProgressBar；否则跑马灯）
// =============================================================================

#pragma once

#include <imgui.h>

#include "app/view/theme/ui_theme.h"

namespace moleculens::app::widgets {

inline void loadingBar(float progress, const ImVec2& size = ImVec2(-1, 8)) {
    UITheme::loadingBar(progress, size);
}

} // namespace moleculens::app::widgets
