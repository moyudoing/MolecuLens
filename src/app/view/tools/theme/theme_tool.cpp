// =============================================================================
// MolecuLens · app · view/tools/theme/theme_tool.cpp —— 主题设置工具实现
// =============================================================================

#include "app/view/tools/theme/theme_tool.h"

#include "app/view/theme/ui_theme.h"
#include <imgui.h>

namespace moleculens::app {

void ThemeTool::render(DataManager* dm, MolScene* scene) {
    (void)dm;
    (void)scene;

    UITheme::sectionHeader("配色方案");

    // 获取当前主题
    selectedTheme_ = UITheme::currentTheme();

    // 主题选择器
    const char* themeNames[] = {
        "东京之夜 (Tokyo Night)",
        "德古拉 (Dracula)",
        "北欧风 (Nord)",
        "暗黑一号 (One Dark)",
        "日光暗色 (Solarized Dark)",
    };

    int currentIdx = static_cast<int>(selectedTheme_);

    ImGui::Text("当前主题");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

    if (ImGui::Combo("##theme_selector", &currentIdx, themeNames,
                     static_cast<int>(UIThemePreset::Count))) {
        selectedTheme_ = static_cast<UIThemePreset>(currentIdx);
        UITheme::setTheme(selectedTheme_);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 主题预览
    UITheme::sectionHeader("配色预览");

    // 获取当前主题配色
    ThemeColors colors = getThemeColors(selectedTheme_);

    auto colorSwatch = [](const char* label, const ImVec4& color) {
        ImGui::Text("%s", label);
        ImGui::SameLine(120);
        ImGui::ColorButton(("##" + std::string(label)).c_str(), color,
                          ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                          ImVec2(100, 20));
    };

    colorSwatch("背景色", colors.bg);
    colorSwatch("表面色", colors.surface);
    colorSwatch("边框色", colors.frame);
    colorSwatch("主题色", colors.accent);
    colorSwatch("高亮色", colors.accentHi);
    colorSwatch("成功色", colors.ok);
    colorSwatch("警告色", colors.warn);
    colorSwatch("危险色", colors.danger);
    colorSwatch("文字色", colors.text);
    colorSwatch("次要文字", colors.textMuted);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 组件预览
    UITheme::sectionHeader("组件预览");

    if (UITheme::primaryButton("主按钮", ImVec2(-1, 0))) {
        // 预览按钮
    }
    ImGui::Spacing();

    if (UITheme::secondaryButton("次按钮", ImVec2(-1, 0))) {
        // 预览按钮
    }
    ImGui::Spacing();

    UITheme::badge("徽章", colors.accent);
    ImGui::SameLine();
    UITheme::badge("成功", colors.ok);
    ImGui::SameLine();
    UITheme::badge("警告", colors.warn);
    ImGui::SameLine();
    UITheme::badge("危险", colors.danger);

    ImGui::Spacing();
    UITheme::helpMarker("这是一个帮助提示");

    ImGui::Spacing();
    UITheme::loadingBar(0.65f, ImVec2(-1, 8));

    ImGui::Spacing();
    ImGui::TextUnformatted("这是普通文字");
    ImGui::TextDisabled("这是禁用文字");
}

} // namespace moleculens::app
