// =============================================================================
// MolecuLens · app · view/theme/ui_theme.cpp —— 设计系统实现
// 规格来源：主方案 §7.6（配色映射；动画引擎；组件库）
// 新增：多主题支持
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/theme/ui_theme.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

#include <imgui_internal.h>

#include "spdlog/spdlog.h"

namespace moleculens::app {

// 全局主题配色（可动态切换）
namespace ui {
ImVec4 C_BG(0.102f, 0.106f, 0.149f, 1.0f);
ImVec4 C_SURFACE(0.122f, 0.137f, 0.208f, 1.0f);
ImVec4 C_FRAME(0.165f, 0.180f, 0.259f, 1.0f);
ImVec4 C_ACCENT(0.478f, 0.635f, 0.969f, 1.0f);
ImVec4 C_ACCENT_HI(0.490f, 0.812f, 1.000f, 1.0f);
ImVec4 C_OK(0.620f, 0.808f, 0.416f, 1.0f);
ImVec4 C_WARN(0.878f, 0.686f, 0.408f, 1.0f);
ImVec4 C_DANGER(0.969f, 0.463f, 0.557f, 1.0f);
ImVec4 C_TEXT(0.753f, 0.792f, 0.961f, 1.0f);
ImVec4 C_TEXT_MUTED(0.337f, 0.373f, 0.537f, 1.0f);
} // namespace ui

bool UITheme::fontsLoaded_ = false;
UIThemePreset UITheme::currentTheme_ = UIThemePreset::TokyoNight;
UIThemePreset UITheme::lastDarkTheme_ = UIThemePreset::TokyoNight;
UIThemePreset UITheme::lastLightTheme_ = UIThemePreset::Light;
ImFont* UITheme::fontDefault_ = nullptr;
ImFont* UITheme::fontTitle_ = nullptr;
ImFont* UITheme::fontSmall_ = nullptr;

float& UITheme::gTime() {
    static float t = 0.0f;
    return t;
}

void UITheme::advanceTime(float dt) {
    gTime() += dt;
    if (gTime() > 1e9f) {
        gTime() = 0.0f;
    }
}

bool UITheme::setTheme(UIThemePreset preset) {
    if (preset == currentTheme_) {
        return false;   // 幂等：调用点无需自行判重（启动加载/UI 每帧回读都会调用）
    }
    currentTheme_ = preset;
    // 记住该模式的最近选择，供"白天/暗黑"切换回到用户真正用过的那套
    if (themePresetIsDark(preset)) {
        lastDarkTheme_ = preset;
    } else {
        lastLightTheme_ = preset;
    }
    ThemeColors colors = getThemeColors(preset);

    // 更新全局配色
    ui::C_BG = colors.bg;
    ui::C_SURFACE = colors.surface;
    ui::C_FRAME = colors.frame;
    ui::C_ACCENT = colors.accent;
    ui::C_ACCENT_HI = colors.accentHi;
    ui::C_OK = colors.ok;
    ui::C_WARN = colors.warn;
    ui::C_DANGER = colors.danger;
    ui::C_TEXT = colors.text;
    ui::C_TEXT_MUTED = colors.textMuted;

    // 重新应用主题到 ImGui
    applyTheme();

    spdlog::info("[ui] 主题切换: {} ({})", themePresetNameZh(preset),
                 themePresetKey(preset));
    return true;
}

UIThemePreset UITheme::currentTheme() {
    return currentTheme_;
}

const char* UITheme::themeKey() {
    return themePresetKey(currentTheme_);
}

bool UITheme::isDarkTheme() {
    return themePresetIsDark(currentTheme_);
}

void UITheme::toggleDarkLight() {
    // 切到"另一模式"里最近用过的那套（不是固定的某一对）
    setTheme(isDarkTheme() ? lastLightTheme_ : lastDarkTheme_);
}

bool UITheme::setThemeByKey(const std::string& key) {
    UIThemePreset preset = currentTheme_;
    if (!themePresetFromKey(key, &preset)) {
        spdlog::warn("[ui] 未知主题键 '{}'（配置可能来自旧版本）—— 保持当前主题 {}", key,
                     themePresetKey(currentTheme_));
        return false;
    }
    setTheme(preset);
    return true;
}

void UITheme::applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // 基础面板/窗口
    colors[ImGuiCol_WindowBg] = ui::C_BG;
    colors[ImGuiCol_ChildBg] = ui::C_SURFACE;
    colors[ImGuiCol_PopupBg] = ImVec4(ui::C_SURFACE.x, ui::C_SURFACE.y, ui::C_SURFACE.z,
                                      0.98f);
    colors[ImGuiCol_Border] = ui::C_FRAME;
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = ui::C_FRAME;
    colors[ImGuiCol_FrameBgHovered] = ImVec4(ui::C_FRAME.x * 1.2f, ui::C_FRAME.y * 1.2f,
                                             ui::C_FRAME.z * 1.2f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ui::C_ACCENT;
    colors[ImGuiCol_TitleBg] = ui::C_SURFACE;
    colors[ImGuiCol_TitleBgActive] = ui::C_SURFACE;
    colors[ImGuiCol_TitleBgCollapsed] = ui::C_BG;
    colors[ImGuiCol_MenuBarBg] = ui::C_SURFACE;
    colors[ImGuiCol_ScrollbarBg] = ui::C_BG;
    colors[ImGuiCol_ScrollbarGrab] = ui::C_FRAME;
    colors[ImGuiCol_ScrollbarGrabHovered] = ui::C_TEXT_MUTED;
    colors[ImGuiCol_ScrollbarGrabActive] = ui::C_ACCENT;
    colors[ImGuiCol_CheckMark] = ui::C_ACCENT_HI;
    colors[ImGuiCol_SliderGrab] = ui::C_ACCENT;
    colors[ImGuiCol_SliderGrabActive] = ui::C_ACCENT_HI;
    colors[ImGuiCol_Button] = ImVec4(ui::C_FRAME.x, ui::C_FRAME.y, ui::C_FRAME.z, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ui::C_ACCENT;
    colors[ImGuiCol_ButtonActive] = ui::C_ACCENT_HI;
    colors[ImGuiCol_Header] = ui::C_FRAME;
    colors[ImGuiCol_HeaderHovered] = ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z,
                                            0.4f);
    colors[ImGuiCol_HeaderActive] = ui::C_ACCENT;
    colors[ImGuiCol_Separator] = ui::C_FRAME;
    colors[ImGuiCol_SeparatorHovered] = ui::C_TEXT_MUTED;
    colors[ImGuiCol_SeparatorActive] = ui::C_ACCENT;
    colors[ImGuiCol_ResizeGrip] = ui::C_FRAME;
    colors[ImGuiCol_ResizeGripHovered] = ui::C_TEXT_MUTED;
    colors[ImGuiCol_ResizeGripActive] = ui::C_ACCENT;
    colors[ImGuiCol_Tab] = ui::C_SURFACE;
    colors[ImGuiCol_TabHovered] = ui::C_ACCENT;
    colors[ImGuiCol_TabSelected] = ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z,
                                          0.25f);
    colors[ImGuiCol_TabSelectedOverline] = ui::C_ACCENT;
    colors[ImGuiCol_TabDimmed] = ui::C_BG;
    colors[ImGuiCol_TabDimmedSelected] = ui::C_SURFACE;
    colors[ImGuiCol_TabDimmedSelectedOverline] = ui::C_TEXT_MUTED;
    colors[ImGuiCol_Text] = ui::C_TEXT;
    colors[ImGuiCol_TextDisabled] = ui::C_TEXT_MUTED;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z,
                                             0.35f);
    colors[ImGuiCol_PlotHistogram] = ui::C_ACCENT;
    colors[ImGuiCol_PlotHistogramHovered] = ui::C_ACCENT_HI;
    colors[ImGuiCol_PlotLines] = ui::C_ACCENT;
    colors[ImGuiCol_PlotLinesHovered] = ui::C_ACCENT_HI;
    colors[ImGuiCol_DragDropTarget] = ui::C_ACCENT_HI;
    colors[ImGuiCol_NavHighlight] = ui::C_ACCENT_HI;
    colors[ImGuiCol_DockingPreview] = ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z,
                                             0.5f);
    colors[ImGuiCol_DockingEmptyBg] = ui::C_BG;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);

    // 统一圆角/边距（主方案 §7.6：窗口 8px / 控件 6px / 零边框 / WindowPadding 14px）
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(10, 5);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
}

bool UITheme::loadFont(const char* fontPath) {
    if (fontsLoaded_) {
        return true;
    }
    ImGuiIO& io = ImGui::GetIO();
    // 中文 Glyph Ranges（主方案 §7.6；默认字体 Noto Sans SC 子集，见 data/font/README.txt）
    // 注意：ranges 必须持久——imgui 的字体配置保存 GlyphRanges 指针（不复制），
    // 局部数组销毁后悬垂会导致运行期断言（imgui_draw.cpp "glyph range array
    // persistent"）。static 保证生命周期与进程一致。
    // 范围用"常用简体字"（~3500 字）而非全量（~2.5 万字形）：
    // 全量构建 atlas 巨大（全量 CJK 可达数十 MB 纹理），启动明显卡顿。
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        // 界面符号（窗口按钮/标题等），Default/中文常用范围未覆盖会渲染成 "?"
        builder.AddChar(0x2013);   // – en dash
        builder.AddChar(0x2014);   // — em dash
        builder.AddChar(0x00D7);   // × 乘号（关闭按钮）
        builder.AddChar(0x00B7);   // · 间隔号
        builder.AddChar(0x2022);   // • 项目符号
        // 新默认字体（Noto Sans SC 子集）已含这两个字形（旧 OPPO_B 没有，只能改文案
        // 或用绘制列表）；module 说明里的 "→" 与折叠三角 "▾" 此前是方块，这里补上。
        builder.AddChar(0x2192);   // → 箭头
        builder.AddChar(0x25BE);   // ▾ 折叠指示三角
        // 这里不再 AddChar(0x25A1/0x25B6/0x23F8)：旧字体 OPPO_B 在 U+2000..U+2BFF
        // 区间只有 22 个码点，没有任何几何符号——加进范围也拿不到字形，只会让 imgui
        // 记一个空槽仍渲染成方块；窗口按钮与体系展开符因此都改为用绘制列表画
        // （见 renderMainMenuBar / mol_list_tool）。新字体（Noto Sans SC 子集）已覆盖
        // 这些符号，但绘制列表方案更清晰且不增 atlas，保持不变。
        // 界面文案里落在"常用简体 2500 字"之外的汉字——该表是 imgui 内置的
        // 固定子集，下列字不在其中，不补就会渲染成方块（"渲染设置"的"渲"）。
        // 新增界面文案后如有方块字，按同样办法补在这里。
        //
        // 这份清单不再靠肉眼发现：tools/check_glyph_coverage.py 扫全部界面字符串
        // 字面量，与 imgui 实际加载的范围表求差，报出未覆盖的码点（本轮据此补入
        // 「拓枚耦」——"拓扑"/"枚举"/"解耦"此前一直在渲染成方块）。
        // ⚠ 这串字是**字体字形数据**，不是界面文案 —— 绝不能过 tr()。
        //   过 tr() 后若该串被收进翻译表，英文模式下 AddText 收到的是英文，
        //   中文字形根本不会被加载（切回中文或日志/文件名里的汉字全变方块）。
        //   i18n 批量工具已把 *.h 与字形清单列入跳过项，此处手写还原以正视听。
        // 「擎」「氢」来自本轮新增文案（"本引擎…" / "需要氢键判定"）—— 同样不在
        // 常用字表内。补字前务必用 tools/check_font_glyphs.py 确认字体确有该字形，
        // 否则补进范围只会得到空槽、仍旧是方块。
        builder.AddText("屏帧徽栅渲溶糙轴辑逻钮阈拓枚耦擎氢廓离嵌拟棱浏锚菱叠");
        // ⚠ 补字**只对字体真有字形的码点有效**。旧字体 OPPO_B 只有 7251 个码点，
        // 几何符号/箭头/希腊字母基本没有（Δ ← → 都不在 cmap 里）→ 补进范围只会
        // 让 imgui 记一个空槽，仍旧渲染成方块。这类字符当时必须**改文案**：
        // 界面一律用 ASCII "->" 而不是 "→"，"dt" 而不是 "Δt"。
        // 现在的默认字体（Noto Sans SC 子集）已覆盖箭头/希腊字母/Å，这些规避文案
        // 仍保留（不改动界面），如需使用可直接 AddChar——判定见 tools/check_font_glyphs.py。
        builder.BuildRanges(&ranges);
    }

    // 字体候选：项目路径 → exe 旁 font/ → 系统字体回退（data/font/README.txt）
    std::string path = fontPath ? fontPath : "";
    auto fileExists = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return f.good();
    };
    if (!path.empty() && !fileExists(path)) {
        path.clear();
    }
    if (path.empty()) {
#ifdef _WIN32
        // 候选：项目 data 目录（含带空格原名）→ exe 目录 → 系统字体
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const std::string exeDir = std::filesystem::path(exePath).parent_path().string();
        const std::string cands[] = {
            "data/font/NotoSansSC-Medium-subset.ttf",
            "font/NotoSansSC-Medium-subset.ttf",
            exeDir + "/font/NotoSansSC-Medium-subset.ttf",
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/simhei.ttf",
        };
        for (const auto& cand : cands) {
            if (fileExists(cand)) {
                path = cand;
                break;
            }
        }
#else
        for (const auto& cand : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
            if (fileExists(cand)) {
                path = cand;
                break;
            }
        }
#endif
    }
    if (path.empty()) {
        spdlog::warn("[ui] no CJK font found, using default glyphs");
        return false;
    }

    // 字号：16/20/13 → 18/22/14。1920×1080 下 16 px 中文偏小，密集面板上
    // "看着不清晰"主要来自字号而非抗锯齿；18 px 在同样布局里仍放得下。
    // RasterizerMultiply 略微加粗笔画，改善浅色主题下细字发虚的问题。
    //
    // 缺字回退：把系统字体（微软雅黑）**合并**进同一 ImFont（MergeMode 会并入
    // "最近一次 AddFont"），只补拉丁补充/希腊/箭头几个小区间，不引入整套 CJK。
    // 默认字体 Noto Sans SC 子集已含 Å(U+00C5)/希腊字母/箭头，这里保留合并仅作
    // 兜底（换回旧字体或用户自备缺字字体时仍能显示）。
    // 因此必须"主字体 + 回退"成对添加，不能先把三档主字体都加完。
    const char* fallback = nullptr;
    for (const char* cand : {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyhbd.ttc",
                             "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}) {
        if (fileExists(cand)) {
            fallback = cand;
            break;
        }
    }
    static ImVector<ImWchar> fallbackRanges;
    if (fallback != nullptr && fallbackRanges.empty()) {
        static const ImWchar kFallback[] = {
            0x00A0, 0x00FF,   // 拉丁补充：Å × · ° ± 等
            0x0370, 0x03FF,   // 希腊字母：Δ α β γ λ 等
            0x2190, 0x21FF,   // 箭头：← → ↑ ↓
            0x2200, 0x22FF,   // 数学算子：≈ ≤ ≥ 等
            0,
        };
        // ImVector 没有 assign：逐项 push（含 0 终止符；ImGui 读的是 Data 指针）
        for (size_t i = 0; i < sizeof(kFallback) / sizeof(kFallback[0]); ++i) {
            fallbackRanges.push_back(kFallback[i]);
        }
    }
    const auto addFontWithFallback = [&](float size, ImFont** out) -> ImFont* {
        ImFontConfig cfg;
        cfg.RasterizerMultiply = 1.06f;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg, ranges.Data);
        if (f != nullptr && fallback != nullptr) {
            ImFontConfig merge;
            merge.MergeMode = true;   // 并入上一个 AddFont 的字体
            merge.RasterizerMultiply = 1.06f;
            io.Fonts->AddFontFromFileTTF(fallback, size, &merge, fallbackRanges.Data);
        }
        if (out != nullptr) {
            *out = f;
        }
        return f;
    };

    ImFont* font = addFontWithFallback(18.0f, &fontDefault_);
    if (!font) {
        return false;
    }
    io.FontDefault = font;
    addFontWithFallback(22.0f, &fontTitle_);
    addFontWithFallback(14.0f, &fontSmall_);

    fontsLoaded_ = true;
    spdlog::info("[ui] font loaded: {} (18/22/14 px{})", path,
                 fallback != nullptr ? ", with fallback" : "");
    if (fallback != nullptr) {
        spdlog::info("[ui] font fallback merged: {} (Å / Greek / arrows)", fallback);
    }
    return true;
}

float UITheme::hoverProgress(ImGuiID id, bool hovered) {
    static std::unordered_map<ImGuiID, float> cache;
    float& v = cache[id];
    const float target = hovered ? 1.0f : 0.0f;
    v += (target - v) * 0.18f;
    if (std::fabs(v - target) < 0.005f) {
        v = target;
    }
    if (cache.size() > 256) {
        cache.clear();
    }
    return easeOutCubic(v);
}

// —— 组件库 ——

void UITheme::sectionHeader(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::C_ACCENT_HI);
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::C_TEXT);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void UITheme::badge(const char* text, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
    ImGui::SmallButton(text);
    ImGui::PopStyleColor(4);
}

bool UITheme::primaryButton(const char* label, const ImVec2& size) {
    const ImGuiID id = ImGui::GetID(label);
    const bool hovered = ImGui::IsItemHovered();
    (void)hovered;
    const float p = hoverProgress(id, ImGui::IsItemHovered());
    const ImVec4 base = ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z,
                               0.7f + 0.3f * p);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::C_ACCENT_HI);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    return clicked;
}

bool UITheme::secondaryButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ui::C_SURFACE);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(ui::C_ACCENT.x, ui::C_ACCENT.y, ui::C_ACCENT.z, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::C_ACCENT);
    // 描边按钮（主方案 §7.6：secondaryButton 描边）
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::C_TEXT_MUTED);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

void UITheme::helpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void UITheme::loadingBar(float progress, const ImVec2& size) {
    if (progress >= 0.0f) {
        // 确定进度
        ImGui::ProgressBar(progress, size);
        return;
    }
    // 不确定（跑马灯：正弦滑动高亮段，主方案 §7.5）
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float t = std::fmod(gTime() * 0.4f, 1.0f);
    const float seg = size.x * 0.25f;
    const float x0 = pos.x + t * (size.x + seg) - seg;
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(38, 42, 61, 255));
    draw->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x0 + seg, pos.y + size.y),
                        IM_COL32(122, 162, 247, 255));
    ImGui::Dummy(size);
}

} // namespace moleculens::app
