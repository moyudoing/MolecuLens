// =============================================================================
// MolecuLens · app · view/theme/ui_theme_presets.h —— UI 主题预设
// 功能：多套配色方案支持（Tokyo Night / Dracula / Nord / One Dark / Solarized）
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>   // uint8_t（枚举底层类型）
#include <string>
#include <string_view>
#include <imgui.h>

namespace moleculens::app {

enum class UIThemePreset : uint8_t {
    // —— 暗色 ——
    TokyoNight,
    Dracula,
    Nord,
    OneDark,
    SolarizedDark,
    // —— 亮色 ——（"白天/暗黑"切换用；参考图是亮色原生灰风格）
    Light,
    SolarizedLight,
    Count,
};

// 主题预设总数（下拉框项数 / 遍历上界；与 Count 同源，避免各处再写魔数）
inline constexpr int kThemePresetCount = static_cast<int>(UIThemePreset::Count);

// 预设下标 ↔ 枚举（下拉框用整数下标，二者必须由本函数对转，
// 不可直接 static_cast —— 名称数组与枚举顺序一旦脱节就是"选到的名字与
// 生效的主题对不上"，且不会编译报错）
inline constexpr UIThemePreset themePresetAt(int index) {
    return (index < 0 || index >= kThemePresetCount) ? UIThemePreset::TokyoNight
                                                     : static_cast<UIThemePreset>(index);
}

// 稳定持久化键（写入 settings.json）。
// 不落盘枚举序号：日后增删/调整预设顺序会让旧配置指向**另一个**主题，
// 且这种错位完全静默。键名只在预设被重命名时才需要迁移。
inline const char* themePresetKey(UIThemePreset preset) {
    switch (preset) {
        case UIThemePreset::TokyoNight:    return "tokyo-night";
        case UIThemePreset::Dracula:       return "dracula";
        case UIThemePreset::Nord:          return "nord";
        case UIThemePreset::OneDark:       return "one-dark";
        case UIThemePreset::SolarizedDark: return "solarized-dark";
        case UIThemePreset::Light:         return "light";
        case UIThemePreset::SolarizedLight: return "solarized-light";
        default:                           return "tokyo-night";
    }
}

// 键 → 预设。未知键（旧版本写下的名字/手改过的配置）返回 false，
// 由调用方决定回退到默认主题，而不是静默取到某个不相干的配色。
inline bool themePresetFromKey(std::string_view key, UIThemePreset* out) {
    for (int i = 0; i < kThemePresetCount; ++i) {
        const auto p = themePresetAt(i);
        if (key == themePresetKey(p)) {
            if (out) {
                *out = p;
            }
            return true;
        }
    }
    return false;
}

// 英文名
inline const char* themePresetName(UIThemePreset preset) {
    switch (preset) {
        case UIThemePreset::TokyoNight:    return "Tokyo Night";
        case UIThemePreset::Dracula:       return "Dracula";
        case UIThemePreset::Nord:          return "Nord";
        case UIThemePreset::OneDark:       return "One Dark";
        case UIThemePreset::SolarizedDark: return "Solarized Dark";
        case UIThemePreset::Light:         return "Light";
        case UIThemePreset::SolarizedLight: return "Solarized Light";
        default:                           return "Unknown";
    }
}

// 中文名
inline const char* themePresetNameZh(UIThemePreset preset) {
    switch (preset) {
        case UIThemePreset::TokyoNight:    return "东京之夜";
        case UIThemePreset::Dracula:       return "德古拉";
        case UIThemePreset::Nord:          return "北欧风";
        case UIThemePreset::OneDark:       return "暗黑一号";
        case UIThemePreset::SolarizedDark: return "日光暗色";
        case UIThemePreset::Light:         return "浅色";
        case UIThemePreset::SolarizedLight: return "日光浅色";
        default:                           return "未知";
    }
}

// 下拉框显示名："东京之夜 (Tokyo Night)"。由枚举派生 —— 此前 theme_tool 手写
// 了一份同序数组，是名称与枚举脱节的又一处隐患。
inline std::string themePresetDisplayName(UIThemePreset preset) {
    return std::string(themePresetNameZh(preset)) + " (" + themePresetName(preset) + ")";
}

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
    // 该预设是暗色还是亮色 —— 供"白天/暗黑"一键切换挑对应模式的主题。
    // 默认 true：既有 5 套全是暗色，故上面的 return {...} 不必逐条改动
    // （聚合初始化未给出的成员取这里的默认值）；亮色预设显式传 false。
    bool dark = true;
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

        // —— 亮色主题：参考图（VMD）是浅灰原生风格 ——
        case UIThemePreset::Light:
            return {
                ImVec4(0.941f, 0.941f, 0.949f, 1.0f),  // bg #F0F0F2（浅灰底）
                ImVec4(0.980f, 0.980f, 0.984f, 1.0f),  // surface #FAFAFB（面板更亮）
                ImVec4(0.804f, 0.808f, 0.827f, 1.0f),  // frame #CDCED3（输入框/边框）
                ImVec4(0.129f, 0.400f, 0.729f, 1.0f),  // accent #2166BA（深蓝，浅底上可读）
                ImVec4(0.086f, 0.294f, 0.557f, 1.0f),  // accentHi #164A8E（更深一档）
                ImVec4(0.180f, 0.510f, 0.243f, 1.0f),  // ok #2E823E
                ImVec4(0.647f, 0.435f, 0.043f, 1.0f),  // warn #A56F0B
                ImVec4(0.749f, 0.153f, 0.153f, 1.0f),  // danger #BF2727
                ImVec4(0.106f, 0.114f, 0.133f, 1.0f),  // text #1B1D22（深灰字）
                ImVec4(0.420f, 0.439f, 0.478f, 1.0f),  // textMuted #6B707A
                false,                                  // 亮色
            };

        case UIThemePreset::SolarizedLight:
            return {
                ImVec4(0.992f, 0.965f, 0.890f, 1.0f),  // bg #FDF6E3
                ImVec4(0.933f, 0.910f, 0.835f, 1.0f),  // surface #EEE8D5
                ImVec4(0.576f, 0.631f, 0.631f, 1.0f),  // frame #93A1A1
                ImVec4(0.149f, 0.545f, 0.824f, 1.0f),  // accent #268BD2
                ImVec4(0.424f, 0.443f, 0.769f, 1.0f),  // accentHi #6C71C4
                ImVec4(0.522f, 0.600f, 0.000f, 1.0f),  // ok #859900
                ImVec4(0.710f, 0.537f, 0.000f, 1.0f),  // warn #B58900
                ImVec4(0.863f, 0.196f, 0.184f, 1.0f),  // danger #DC322F
                ImVec4(0.345f, 0.431f, 0.459f, 1.0f),  // text #586E75
                ImVec4(0.576f, 0.631f, 0.631f, 1.0f),  // textMuted #93A1A1
                false,                                  // 亮色
            };

        default:
            return getThemeColors(UIThemePreset::TokyoNight);
    }
}

// 预设是否为亮色（"白天/暗黑"切换用）
inline bool themePresetIsDark(UIThemePreset preset) {
    return getThemeColors(preset).dark;
}

} // namespace moleculens::app
