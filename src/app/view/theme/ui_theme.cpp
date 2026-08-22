// =============================================================================
// MolecuLens · app · view/theme/ui_theme.cpp —— 设计系统实现
// 规格来源：主方案 §7.6（配色映射；动画引擎；组件库）
// =============================================================================

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

bool UITheme::fontsLoaded_ = false;

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
    // 中文 Glyph Ranges（主方案 §7.6：OPPO B.ttf 中文）
    // 注意：ranges 必须持久——imgui 的字体配置保存 GlyphRanges 指针（不复制），
    // 局部数组销毁后悬垂会导致运行期断言（imgui_draw.cpp "glyph range array
    // persistent"）。static 保证生命周期与进程一致。
    // 范围用"常用简体字"（~3500 字）而非全量（~2.5 万字形）：
    // 全量构建 atlas 巨大（OPPO B 全量中文可达数十 MB 纹理），启动明显卡顿。
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
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
            "data/font/OPPO B.ttf",
            "data/font/OPPO_B.ttf",
            "font/OPPO_B.ttf",
            exeDir + "/font/OPPO_B.ttf",
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

    ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, nullptr, ranges.Data);
    if (!font) {
        return false;
    }
    // 三档字号（主方案 §7.6：16/20/13）
    io.FontDefault = font;
    ImFont* fontTitle = io.Fonts->AddFontFromFileTTF(path.c_str(), 20.0f, nullptr,
                                                     ranges.Data);
    ImFont* fontSmall = io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f, nullptr,
                                                     ranges.Data);
    (void)fontTitle;
    (void)fontSmall;
    fontsLoaded_ = true;
    spdlog::info("[ui] font loaded: {}", path);
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
