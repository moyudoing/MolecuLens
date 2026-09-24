// =============================================================================
// MolecuLens · app · view/theme/ui_theme_presets.h —— UI 主题预设
// 功能：多套配色方案支持（Tokyo Night / Dracula / Nord / One Dark / Solarized）
// =============================================================================

#pragma once

#include <cstdint>   // uint8_t（枚举底层类型）
#include <imgui.h>

namespace moleculens::app {

enum class UIThemePreset : uint8_t {
    TokyoNight,
    Dracula,
    Nord,
    OneDark,
    SolarizedDark,
    Count,
};

// 主题配色定义
struct ThemeColors {
    ImVec4 bg;
    ImVec4 surface;
    ImVec4 frame;
    ImVec4 accent;
    ImVec4 accentHi;
    ImVec4 ok;
    ImVec4 warn;
    ImVec4 danger;
    ImVec4 text;
    ImVec4 textMuted;
};

// 获取主题配色
inline ThemeColors getThemeColors(UIThemePreset preset) {
    switch (preset) {
        case UIThemePreset::TokyoNight:
            return {
                ImVec4(0.102f, 0.106f, 0.149f, 1.0f),  // bg #1A1B26
                ImVec4(0.122f, 0.137f, 0.208f, 1.0f),  // surface #1F2335
                ImVec4(0.165f, 0.180f, 0.259f, 1.0f),  // frame #2A2E42
                ImVec4(0.478f, 0.635f, 0.969f, 1.0f),  // accent #7AA2F7
                ImVec4(0.490f, 0.812f, 1.000f, 1.0f),  // accentHi #7DCFFF
                ImVec4(0.620f, 0.808f, 0.416f, 1.0f),  // ok #9ECE6A
                ImVec4(0.878f, 0.686f, 0.408f, 1.0f),  // warn #E0AF68
                ImVec4(0.969f, 0.463f, 0.557f, 1.0f),  // danger #F7768E
                ImVec4(0.753f, 0.792f, 0.961f, 1.0f),  // text #C0CAF5
                ImVec4(0.337f, 0.373f, 0.537f, 1.0f),  // textMuted #565F89
            };

        case UIThemePreset::Dracula:
            return {
                ImVec4(0.157f, 0.165f, 0.212f, 1.0f),  // bg #282A36
                ImVec4(0.227f, 0.239f, 0.310f, 1.0f),  // surface #3A3D4F
                ImVec4(0.278f, 0.290f, 0.369f, 1.0f),  // frame #474A5E
                ImVec4(0.741f, 0.576f, 0.976f, 1.0f),  // accent #BD93F9 (purple)
                ImVec4(1.000f, 0.471f, 0.776f, 1.0f),  // accentHi #FF79C6 (pink)
                ImVec4(0.314f, 0.980f, 0.482f, 1.0f),  // ok #50FA7B (green)
                ImVec4(1.000f, 0.714f, 0.537f, 1.0f),  // warn #FFB86C (orange)
                ImVec4(1.000f, 0.333f, 0.333f, 1.0f),  // danger #FF5555 (red)
                ImVec4(0.945f, 0.980f, 1.000f, 1.0f),  // text #F8F8F2
                ImVec4(0.388f, 0.447f, 0.643f, 1.0f),  // textMuted #6272A4
            };

        case UIThemePreset::Nord:
            return {
                ImVec4(0.180f, 0.204f, 0.251f, 1.0f),  // bg #2E3440
                ImVec4(0.231f, 0.259f, 0.322f, 1.0f),  // surface #3B4252
                ImVec4(0.298f, 0.337f, 0.416f, 1.0f),  // frame #4C566A
                ImVec4(0.533f, 0.753f, 0.816f, 1.0f),  // accent #88C0D0 (frost cyan)
                ImVec4(0.569f, 0.776f, 0.761f, 1.0f),  // accentHi #91C6C2 (frost teal)
                ImVec4(0.639f, 0.745f, 0.549f, 1.0f),  // ok #A3BE8C (green)
                ImVec4(0.922f, 0.796f, 0.545f, 1.0f),  // warn #EBCB8B (yellow)
                ImVec4(0.749f, 0.380f, 0.416f, 1.0f),  // danger #BF616A (red)
                ImVec4(0.925f, 0.937f, 0.953f, 1.0f),  // text #ECEFF4
                ImVec4(0.373f, 0.420f, 0.518f, 1.0f),  // textMuted #5F6B84
            };

        case UIThemePreset::OneDark:
            return {
                ImVec4(0.157f, 0.169f, 0.212f, 1.0f),  // bg #282C34
                ImVec4(0.196f, 0.212f, 0.259f, 1.0f),  // surface #323642
                ImVec4(0.235f, 0.251f, 0.306f, 1.0f),  // frame #3C404E
                ImVec4(0.380f, 0.678f, 0.976f, 1.0f),  // accent #61AFEF (blue)
                ImVec4(0.776f, 0.616f, 0.996f, 1.0f),  // accentHi #C678DD (purple)
                ImVec4(0.596f, 0.761f, 0.490f, 1.0f),  // ok #98C379 (green)
                ImVec4(0.902f, 0.710f, 0.357f, 1.0f),  // warn #E5C07B (yellow)
                ImVec4(0.878f, 0.380f, 0.416f, 1.0f),  // danger #E06C75 (red)
                ImVec4(0.671f, 0.714f, 0.773f, 1.0f),  // text #ABB2BF
                ImVec4(0.329f, 0.365f, 0.424f, 1.0f),  // textMuted #545D6C
            };

        case UIThemePreset::SolarizedDark:
            return {
                ImVec4(0.000f, 0.169f, 0.212f, 1.0f),  // bg #002B36
                ImVec4(0.027f, 0.212f, 0.259f, 1.0f),  // surface #073642
                ImVec4(0.067f, 0.259f, 0.314f, 1.0f),  // frame #11424F
                ImVec4(0.149f, 0.545f, 0.824f, 1.0f),  // accent #268BD2 (blue)
                ImVec4(0.424f, 0.443f, 0.769f, 1.0f),  // accentHi #6C71C4 (violet)
                ImVec4(0.522f, 0.600f, 0.000f, 1.0f),  // ok #859900 (green)
                ImVec4(0.710f, 0.537f, 0.000f, 1.0f),  // warn #B58900 (yellow)
                ImVec4(0.863f, 0.196f, 0.184f, 1.0f),  // danger #DC322F (red)
                ImVec4(0.933f, 0.910f, 0.835f, 1.0f),  // text #EEE8D5
                ImVec4(0.345f, 0.431f, 0.459f, 1.0f),  // textMuted #586E75
            };

        default:
            return getThemeColors(UIThemePreset::TokyoNight);
    }
}

// 获取主题名称
inline const char* getThemeName(UIThemePreset preset) {
    switch (preset) {
        case UIThemePreset::TokyoNight: return "Tokyo Night";
        case UIThemePreset::Dracula: return "Dracula";
        case UIThemePreset::Nord: return "Nord";
        case UIThemePreset::OneDark: return "One Dark";
        case UIThemePreset::SolarizedDark: return "Solarized Dark";
        default: return "Unknown";
    }
}

// 获取主题中文名称
inline const char* getThemeNameZh(UIThemePreset preset) {
    switch (preset) {
        case UIThemePreset::TokyoNight: return "东京之夜";
        case UIThemePreset::Dracula: return "德古拉";
        case UIThemePreset::Nord: return "北欧风";
        case UIThemePreset::OneDark: return "暗黑一号";
        case UIThemePreset::SolarizedDark: return "日光暗色";
        default: return "未知";
    }
}

} // namespace moleculens::app
