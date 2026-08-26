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
    static void setTheme(UIThemePreset preset);
    static UIThemePreset currentTheme();

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
    static ImFont* fontDefault_;
    static ImFont* fontTitle_;
    static ImFont* fontSmall_;
};

} // namespace moleculens::app
