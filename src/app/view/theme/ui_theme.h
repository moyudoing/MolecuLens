// =============================================================================
// MolecuLens · app · view/theme/ui_theme.h —— 设计系统（UITheme）
// 规格来源：主方案 §7.6（Tokyo Night 配色；OPPO B.ttf 中文 Glyph Ranges；
//           三档字号 16/20/13px；动画引擎 hoverProgress easeOutCubic；
//           组件库 sectionHeader/badge/primaryButton/loadingBar/helpMarker）
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include <imgui.h>

namespace moleculens::app {

// Tokyo Night 配色（主方案 §7.6 表）
namespace ui {
inline constexpr ImVec4 C_BG(0.102f, 0.106f, 0.149f, 1.0f);         // #1A1B26
inline constexpr ImVec4 C_SURFACE(0.122f, 0.137f, 0.208f, 1.0f);    // #1F2335
inline constexpr ImVec4 C_FRAME(0.165f, 0.180f, 0.259f, 1.0f);      // #2A2E42
inline constexpr ImVec4 C_ACCENT(0.478f, 0.635f, 0.969f, 1.0f);     // #7AA2F7
inline constexpr ImVec4 C_ACCENT_HI(0.490f, 0.812f, 1.000f, 1.0f);  // #7DCFFF
inline constexpr ImVec4 C_OK(0.620f, 0.808f, 0.416f, 1.0f);         // #9ECE6A
inline constexpr ImVec4 C_WARN(0.878f, 0.686f, 0.408f, 1.0f);       // #E0AF68
inline constexpr ImVec4 C_DANGER(0.969f, 0.463f, 0.557f, 1.0f);     // #F7768E
inline constexpr ImVec4 C_TEXT(0.753f, 0.792f, 0.961f, 1.0f);       // #C0CAF5
inline constexpr ImVec4 C_TEXT_MUTED(0.337f, 0.373f, 0.537f, 1.0f); // #565F89
} // namespace ui

class UITheme {
public:
    // 全局时钟（动画引擎，主方案 §7.6）
    static float& gTime();
    static void advanceTime(float dt);

    // 应用主题（全部 ImGui 控件色映射，主方案 §7.6）
    static void applyTheme();
    // 加载字体（OPPO B.ttf + 中文 Glyph Ranges；可执行目录 font/）
    static bool loadFont(const char* fontPath);

    // 动画：悬停进度（easeOutCubic 缓动；缓存超 256 自动清理）
    static float hoverProgress(ImGuiID id, bool hovered);
    static float easeOutCubic(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }

    // —— 组件库（主方案 §7.6）——
    static void sectionHeader(const char* title);
    static void badge(const char* text, const ImVec4& color);
    static bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
    static bool secondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
    static void helpMarker(const char* text);
    static void loadingBar(float progress, const ImVec2& size);   // <0 不确定（跑马灯）

private:
    static bool fontsLoaded_;
};

} // namespace moleculens::app
