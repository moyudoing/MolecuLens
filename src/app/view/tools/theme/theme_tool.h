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
    ~ThemeTool() override = default;

    void render(DataManager* dm, MolScene* scene) override;

private:
    UIThemePreset selectedTheme_ = UIThemePreset::TokyoNight;
};

} // namespace moleculens::app
