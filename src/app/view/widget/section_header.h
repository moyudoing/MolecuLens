// =============================================================================
// MolecuLens · app · view/widget/section_header.h —— 区块标题（左强调色竖条 + 大标题）
// 规格来源：主方案 §7.6 组件库
// =============================================================================

#pragma once

#include <imgui.h>

#include "app/view/theme/ui_theme.h"

namespace moleculens::app::widgets {

inline void sectionHeader(const char* title) {
    UITheme::sectionHeader(title);
}

} // namespace moleculens::app::widgets
