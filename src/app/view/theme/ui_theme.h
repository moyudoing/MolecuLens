// =============================================================================
// MolecuLens · app · view/theme/ui_theme.h —— 设计系统（UITheme）
// 规格来源：主方案 §7.6（Tokyo Night 配色；OPPO_B.ttf 中文 Glyph Ranges；
//           三档字号 16/20/13px；动画引擎 hoverProgress easeOutCubic；
//           组件库 sectionHeader/badge/primaryButton/loadingBar/helpMarker）
// 新增：多主题支持（Tokyo Night / Dracula / Nord / One Dark / Solarized Dark）
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include <imgui.h>

#include "app/view/theme/ui_theme_presets.h"

namespace moleculens::app {

// 当前主题配色（动态，随主题切换更新）
namespace ui {
extern ImVec4 C_BG;
extern ImVec4 C_SURFACE;
extern ImVec4 C_FRAME;
extern ImVec4 C_ACCENT;
extern ImVec4 C_ACCENT_HI;
extern ImVec4 C_OK;
extern ImVec4 C_WARN;
extern ImVec4 C_DANGER;
extern ImVec4 C_TEXT;
extern ImVec4 C_TEXT_MUTED;
} // namespace ui

class UITheme {
public:
    // 全局时钟（动画引擎，主方案 §7.6）
    static float& gTime();
    static void advanceTime(float dt);

    // 主题管理
    // 设置主题并立即应用到 ImGui。返回 true = 确实发生了切换（配色已更新），
    // false = 与当前主题相同（未做重复的样式重写与日志）。
    static bool setTheme(UIThemePreset preset);
    static UIThemePreset currentTheme();

    // 持久化键（settings.json 中的 "theme" 字段值）。用稳定字符串而非枚举序号：
    // 日后调整预设顺序不会让旧配置指向另一个主题。
    static const char* themeKey();

    // —— 白天 / 暗黑 ——
    // 当前主题是否暗色（由预设自带的 dark 标志决定）
    [[nodiscard]] static bool isDarkTheme();
    // 在"最近用过的暗色预设"与"最近用过的亮色预设"之间切换（各自记忆）。
    // 不硬编码某一对：用户若把暗色换成 Nord、亮色换成日光浅色，切换应在**他选的**
    // 两套之间来回，而不是跳回默认的那对。
    static void toggleDarkLight();
    // 按键恢复主题；键未知（旧配置/手改）返回 false 且**不改动**当前主题，
    // 由调用方决定是否回退到默认。
    static bool setThemeByKey(const std::string& key);

    // 应用主题（全部 ImGui 控件色映射，主方案 §7.6）
    static void applyTheme();
    // 加载字体（OPPO_B.ttf + 中文 Glyph Ranges；可执行目录 font/）
    static bool loadFont(const char* fontPath);

    // 动画：悬停进度（easeOutCubic 缓动；缓存超 256 自动清理）
    static float hoverProgress(ImGuiID id, bool hovered);
    static float easeOutCubic(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }

    // 字体访问（三档字号：16/20/13）
    static ImFont* fontDefault() { return fontDefault_; }
    static ImFont* fontTitle() { return fontTitle_; }
    static ImFont* fontSmall() { return fontSmall_; }

    // —— 组件库（主方案 §7.6）——
    static void sectionHeader(const char* title);
    static void badge(const char* text, const ImVec4& color);
    static bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
    static bool secondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
    static void helpMarker(const char* text);
    static void loadingBar(float progress, const ImVec2& size);   // <0 不确定（跑马灯）

private:
    static bool fontsLoaded_;
    static UIThemePreset currentTheme_;
    // 最近一次选中的暗色/亮色预设（切换时回到它们，而不是固定的一对）
    static UIThemePreset lastDarkTheme_;
    static UIThemePreset lastLightTheme_;
    static ImFont* fontDefault_;
    static ImFont* fontTitle_;
    static ImFont* fontSmall_;
};

} // namespace moleculens::app
