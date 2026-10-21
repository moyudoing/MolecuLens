// =============================================================================
// MolecuLens · app · view/tools/theme/theme_tool.h —— 主题设置工具
// 功能：UI 主题选择器
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"
#include "app/view/theme/ui_theme_presets.h"

namespace moleculens::app {

class ThemeTool : public BaseToolView {
public:
    [[nodiscard]] const char* getTitle() const override { return "主题设置"; }
    // 窗口稳定 ID（与 dock_layout.h 的 panel::kTheme 必须一致；
    // 它参与 Dock 布局持久化，**不可随语言变化**）
    [[nodiscard]] const char* getStableId() const override { return dock::panel::kTheme; }
    ~ThemeTool() override = default;

    void render(DataManager* dm, MolScene* scene) override;

private:
    UIThemePreset selectedTheme_ = UIThemePreset::TokyoNight;
};

} // namespace moleculens::app
