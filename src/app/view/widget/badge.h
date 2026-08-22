// =============================================================================
// MolecuLens · app · view/widget/badge.h —— 徽标组件
// 规格来源：主方案 §7.6 组件库
// =============================================================================

#pragma once

#include <imgui.h>

#include "app/view/theme/ui_theme.h"

namespace moleculens::app::widgets {

inline void badge(const char* text, const ImVec4& color) {
    UITheme::badge(text, color);
}

} // namespace moleculens::app::widgets
