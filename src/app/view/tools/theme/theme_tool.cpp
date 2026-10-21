// =============================================================================
// MolecuLens · app · view/tools/theme/theme_tool.cpp —— 主题设置工具实现
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/tools/theme/theme_tool.h"

#include "app/view/theme/ui_theme.h"
#include <imgui.h>

#include "persist/settings/settings.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

// 主题偏好即时落盘（settings.json 的 "theme" 键）。
// 这里有意的取舍：整份 load→改→save，而不是持有 Application 的设置对象副本——
// 与本文件同一模式的 ViewControl::runLoop 退出保存一致，避免出现两份设置状态
// （正是 export_img_tool 曾经"界面值与生效参数不一致"的成因）。
void persistThemePreference(const char* key) {
    persist::Settings settings;
    auto loaded = persist::SettingsStore::load();
    if (loaded) {
        settings = loaded.value();
    }
    if (settings.theme == key) {
        return;   // 无变化，不写盘（切换同一主题时不该产生磁盘写）
    }
    settings.theme = key;
    auto saved = persist::SettingsStore::save(settings);
    if (!saved) {
        spdlog::warn("[ui] 主题偏好保存失败: {}", saved.errorMessage());
    }
}

} // namespace

void ThemeTool::render(DataManager* dm, MolScene* scene) {
    (void)dm;
    (void)scene;

    // —— 白天 / 暗黑一键切换 ——
    // 放在最前面：这是最高频的开关，而下面的 23→7 项预设列表是精细选择。
    UITheme::sectionHeader(tr("昼夜模式"));
    const bool dark = UITheme::isDarkTheme();
    ImGui::TextUnformatted(dark ? tr("当前：暗黑模式") : tr("当前：白天模式"));
    if (UITheme::secondaryButton(tr(dark ? "切换到白天模式" : "切换到暗黑模式"),
                                 ImVec2(-1, 0))) {
        UITheme::toggleDarkLight();
        persistThemePreference(UITheme::themeKey());
    }
    UITheme::helpMarker("在「白天」与「暗黑」之间切换；各自记住你最近选用的那一套。");

    ImGui::Spacing();
    UITheme::sectionHeader(tr("配色方案"));

    // 获取当前主题（以 UITheme 为准，不镜像本地状态）
    selectedTheme_ = UITheme::currentTheme();

    // 主题选择器：名称/项数由 UIThemePreset 枚举派生。
    // 此前这里手写了一份同序数组 + 硬编码 5 —— 与枚举脱节时"选到的名字与
    // 生效的主题对不上"且不会编译报错，与 styles_tool 的 styleNames/styleTypes
    // 是同一类隐患（那里改用 static_assert 兜底，这里直接消除第二份来源）。
    // 名称由 tr() 翻译（中英双语）；亮/暗在名字后标注，便于在长列表里区分
    std::string themeNames[kThemePresetCount];
    const char* themeNamePtrs[kThemePresetCount];
    for (int i = 0; i < kThemePresetCount; ++i) {
        const auto preset = themePresetAt(i);
        themeNames[i] = std::string(tr(themePresetNameZh(preset))) + " (" +
                        tr(themePresetName(preset)) + ")";
        themeNamePtrs[i] = themeNames[i].c_str();
    }

    int currentIdx = static_cast<int>(selectedTheme_);

    ImGui::Text(tr("当前主题"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

    if (ImGui::Combo("##theme_selector", &currentIdx, themeNamePtrs,
                     kThemePresetCount)) {
        selectedTheme_ = themePresetAt(currentIdx);
        if (UITheme::setTheme(selectedTheme_)) {
            persistThemePreference(UITheme::themeKey());
        }
    }
    UITheme::helpMarker(tr("主题偏好即时写入 settings.json（key: theme），" "下次启动自动恢复；存的是稳定键名而非枚举序号。"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 主题预览
    UITheme::sectionHeader(tr("配色预览"));

    // 获取当前主题配色
    ThemeColors colors = getThemeColors(selectedTheme_);

    auto colorSwatch = [](const char* label, const ImVec4& color) {
        ImGui::Text("%s", label);
        ImGui::SameLine(120);
        ImGui::ColorButton(("##" + std::string(label)).c_str(), color,
                          ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                          ImVec2(100, 20));
    };

    colorSwatch(tr("背景色"), colors.bg);
    colorSwatch(tr("表面色"), colors.surface);
    colorSwatch(tr("边框色"), colors.frame);
    colorSwatch(tr("主题色"), colors.accent);
    colorSwatch(tr("高亮色"), colors.accentHi);
    colorSwatch(tr("成功色"), colors.ok);
    colorSwatch(tr("警告色"), colors.warn);
    colorSwatch(tr("危险色"), colors.danger);
    colorSwatch(tr("文字色"), colors.text);
    colorSwatch(tr("次要文字"), colors.textMuted);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 组件预览
    UITheme::sectionHeader(tr("组件预览"));

    if (UITheme::primaryButton(tr("主按钮"), ImVec2(-1, 0))) {
        // 预览按钮
    }
    ImGui::Spacing();

    if (UITheme::secondaryButton(tr("次按钮"), ImVec2(-1, 0))) {
        // 预览按钮
    }
    ImGui::Spacing();

    UITheme::badge(tr("徽章"), colors.accent);
    ImGui::SameLine();
    UITheme::badge(tr("成功"), colors.ok);
    ImGui::SameLine();
    UITheme::badge(tr("警告"), colors.warn);
    ImGui::SameLine();
    UITheme::badge(tr("危险"), colors.danger);

    ImGui::Spacing();
    UITheme::helpMarker(tr("这是一个帮助提示"));

    ImGui::Spacing();
    UITheme::loadingBar(0.65f, ImVec2(-1, 8));

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("这是普通文字"));
    ImGui::TextDisabled(tr("这是禁用文字"));
}

} // namespace moleculens::app
