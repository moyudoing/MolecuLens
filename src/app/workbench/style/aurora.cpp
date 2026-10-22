// =============================================================================
// MolecuLens v2 · app/workbench/style/aurora.cpp
// =============================================================================

#include "app/workbench/style/aurora.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace moleculens::app::aurora {
namespace {

Palette g;

// 用户自定义的强调色（v2.3）。apply() 会重建 palette，因此这里单独保存覆盖值，
// 在每次 build 之后重新着色——否则切一次深/浅主题就丢回内置蓝。
ImVec4 g_accentOverride;
bool g_hasAccent = false;

ImVec4 mixColor(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void recolorAccent() {
    if (!g_hasAccent) {
        return;
    }
    const ImVec4 a = g_accentOverride;
    g.accent = a;
    // 悬停色：往白色提亮（深色主题提得多一点，保证在深底上仍亮）
    g.accentHi = mixColor(a, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), g.dark ? 0.38f : 0.22f);
    // 淡底：16% 主色 + 84% 面板色（浅/深主题都自然）
    g.accentSoft = ImVec4(a.x * 0.16f + g.panel.x * 0.84f,
                          a.y * 0.16f + g.panel.y * 0.84f,
                          a.z * 0.16f + g.panel.z * 0.84f, 1.0f);
    const float lum = 0.299f * a.x + 0.587f * a.y + 0.114f * a.z;
    g.onAccent = lum > 0.62f ? ImVec4(0.06f, 0.09f, 0.14f, 1.0f)
                             : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

ImVec4 rgb(int r, int g_, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g_) / 255.0f,
                  static_cast<float>(b) / 255.0f, a);
}

void buildLight() {
    Palette p;
    p.dark = false;
    p.bg = rgb(238, 242, 247);
    p.panel = rgb(255, 255, 255);
    p.panelAlt = rgb(248, 250, 253);
    // 浅色主题此前"看不清"的主因：输入框底、边框、辅助文字都太接近白色。
    // 把 sunken 底压深一档、边框加实，textFaint 从 2.5:1 提到 ~4:1。
    p.panelSunken = rgb(235, 241, 248);
    p.border = rgb(205, 216, 230);
    p.borderStrong = rgb(160, 178, 200);
    p.text = rgb(24, 33, 46);
    p.textMuted = rgb(82, 99, 122);
    p.textFaint = rgb(112, 128, 150);
    p.accent = rgb(37, 99, 235);
    p.accentHi = rgb(59, 130, 246);
    p.accentSoft = rgb(219, 234, 254);
    p.onAccent = rgb(255, 255, 255);
    p.selection = rgb(37, 99, 235);
    p.onSelection = rgb(255, 255, 255);
    p.ok = rgb(16, 185, 129);
    p.warn = rgb(245, 158, 11);
    p.danger = rgb(239, 68, 68);
    // 视口空背景：极浅的冷灰蓝渐变，让分子成为唯一视觉焦点
    p.gradTop = rgb(246, 249, 253);
    p.gradBottom = rgb(226, 233, 243);
    g = p;
}

void buildDark() {
    Palette p;
    p.dark = true;
    p.bg = rgb(13, 20, 33);
    p.panel = rgb(22, 32, 50);
    p.panelAlt = rgb(27, 39, 59);
    p.panelSunken = rgb(17, 26, 42);
    p.border = rgb(44, 60, 84);
    p.borderStrong = rgb(71, 92, 122);
    p.text = rgb(232, 238, 247);
    p.textMuted = rgb(162, 177, 198);
    p.textFaint = rgb(124, 140, 164);
    p.accent = rgb(96, 165, 250);
    p.accentHi = rgb(147, 197, 253);
    p.accentSoft = rgb(30, 58, 95);
    p.onAccent = rgb(8, 15, 28);
    p.selection = rgb(37, 99, 235);
    p.onSelection = rgb(255, 255, 255);
    p.ok = rgb(52, 211, 153);
    p.warn = rgb(251, 191, 36);
    p.danger = rgb(248, 113, 113);
    p.gradTop = rgb(19, 29, 46);
    p.gradBottom = rgb(11, 17, 30);
    g = p;
}

} // namespace

const Palette& palette() { return g; }
bool isDark() { return g.dark; }

void setAccent(const ImVec4& accent) {
    g_accentOverride = ImVec4(accent.x, accent.y, accent.z, 1.0f);
    g_hasAccent = true;
    apply(g.dark);   // 重建 ImGui style（按钮/选中态/滑块一起换色）
}

void clearAccent() {
    g_hasAccent = false;
    apply(g.dark);
}

bool hasCustomAccent() { return g_hasAccent; }

ImVec4 accentColor() { return g.accent; }

ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }

void apply(bool dark) {
    if (dark) {
        buildDark();
    } else {
        buildLight();
    }
    recolorAccent();
    const Palette& p = g;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 10.0f;
    s.FrameRounding = 7.0f;
    s.PopupRounding = 10.0f;
    s.ScrollbarRounding = 9.0f;
    s.GrabRounding = 7.0f;
    s.TabRounding = 7.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    // 输入框/下拉/combo 增加 1px 描边：浅色主题里 frame 底与 panel 底几乎同色，
    // 没有边框时控件边界完全看不出来。
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.CellPadding = ImVec2(8.0f, 6.0f);
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 10.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.DisabledAlpha = 0.62f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textFaint;
    c[ImGuiCol_WindowBg] = p.bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = p.panel;
    c[ImGuiCol_Border] = dark ? p.border : rgb(198, 210, 226);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.panelSunken;
    c[ImGuiCol_FrameBgHovered] = dark ? rgb(36, 51, 76) : rgb(236, 242, 249);
    c[ImGuiCol_FrameBgActive] = dark ? rgb(40, 58, 88) : rgb(228, 237, 249);
    c[ImGuiCol_TitleBg] = p.panel;
    c[ImGuiCol_TitleBgActive] = p.panel;
    c[ImGuiCol_TitleBgCollapsed] = p.panel;
    c[ImGuiCol_MenuBarBg] = p.panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = dark ? rgb(55, 74, 101) : rgb(203, 213, 225);
    c[ImGuiCol_ScrollbarGrabHovered] = dark ? rgb(72, 95, 128) : rgb(148, 163, 184);
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    // 浅色主题下原来的 checkmark/slider 直接用 accent，若用户选了浅粉强调色，
    // 复选框勾与选中行几乎看不见。这里把浅色模式的"可交互标记"压暗 35%，
    // 深色模式仍用亮色（在深底上更醒目）。
    const ImVec4 markColor = dark ? p.accent
                                  : mixColor(p.accent, rgb(20, 28, 40), 0.35f);
    c[ImGuiCol_CheckMark] = markColor;
    c[ImGuiCol_SliderGrab] = markColor;
    c[ImGuiCol_SliderGrabActive] = dark ? p.accentHi
                                        : mixColor(p.accentHi, rgb(20, 28, 40), 0.22f);
    c[ImGuiCol_Button] = dark ? p.panelAlt : rgb(244, 247, 251);
    c[ImGuiCol_ButtonHovered] = dark ? rgb(38, 55, 82) : rgb(232, 240, 251);
    c[ImGuiCol_ButtonActive] = p.accentSoft;
    // 选中行/树节点：浅色主题 72% 主色（旧值 22%/55%，浅粉强调色下仍偏淡），
    // 深色 30%。左侧另有 4px 深墨竖条 + 1px 描边（见 renderRepList），保证可读。
    c[ImGuiCol_Header] = mixColor(p.accent, p.panel, dark ? 0.70f : 0.72f);
    c[ImGuiCol_HeaderHovered] = dark ? rgb(38, 58, 90)
                                     : mixColor(p.accent, p.panel, 0.88f);
    c[ImGuiCol_HeaderActive] = mixColor(p.accent, p.panel, dark ? 0.55f : 0.85f);
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_SeparatorHovered] = p.accentHi;
    c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = p.panelAlt;
    c[ImGuiCol_TabHovered] = p.accentSoft;
    c[ImGuiCol_TabSelected] = p.panel;
    c[ImGuiCol_TableHeaderBg] = p.panelSunken;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = p.border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0.02f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.25f);
    c[ImGuiCol_NavHighlight] = p.accent;
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.08f, 0.14f, 0.45f);
}

void toggleDark() { apply(!g.dark); }

void sectionLabel(const char* text, const char* hint) {
    const Palette& p = g;
    ImGui::TextColored(p.textMuted, "%s", text);
    if (hint != nullptr && hint[0] != '\0') {
        // 检查器较窄：提示放不下就换行，避免被裁掉
        const float avail = ImGui::GetContentRegionAvail().x;
        const float need = ImGui::CalcTextSize(hint).x;
        if (need + 8.0f <= avail) {
            ImGui::SameLine();
            ImGui::TextColored(p.textFaint, "%s", hint);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
            ImGui::TextWrapped("%s", hint);
            ImGui::PopStyleColor();
        }
    }
}

bool cardBegin(const char* id, const char* title, const char* hint) {
    const Palette& p = g;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    const bool open = ImGui::BeginChild(
        id, ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (open) {
        if (title != nullptr && title[0] != '\0') {
            ImGui::TextColored(p.text, "%s", title);
            if (hint != nullptr && hint[0] != '\0') {
                const float avail = ImGui::GetContentRegionAvail().x;
                if (ImGui::CalcTextSize(hint).x + 8.0f <= avail) {
                    ImGui::SameLine();
                    ImGui::TextColored(p.textFaint, "%s", hint);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
                    ImGui::TextWrapped("%s", hint);
                    ImGui::PopStyleColor();
                }
            }
            ImGui::Spacing();
        }
    }
    return open;
}

void cardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// —— 应用标识：透镜环 + 三原子分子 ——
// 与 design/logo/app.ico 同一套几何比例：环半径 0.38·size、环厚 0.085·size，
// 中心原子 0.105·size，两个配体 0.078·size，键宽 0.055·size。
void logoMark(ImDrawList* dl, const ImVec2& c, float size, ImU32 ring, ImU32 atom) {
    const float ringR = size * 0.38f;
    const float ringW = std::max(1.0f, size * 0.085f);
    dl->AddCircle(c, ringR, ring, 0, ringW);

    const ImVec2 a0(c.x, c.y - size * 0.015f);
    const ImVec2 a1(c.x - size * 0.150f, c.y + size * 0.145f);
    const ImVec2 a2(c.x + size * 0.150f, c.y + size * 0.145f);
    const float bondW = std::max(1.0f, size * 0.055f);
    dl->AddLine(a0, a1, atom, bondW);
    dl->AddLine(a0, a2, atom, bondW);
    dl->AddCircleFilled(a0, size * 0.105f, atom, 20);
    dl->AddCircleFilled(a1, size * 0.078f, atom, 16);
    dl->AddCircleFilled(a2, size * 0.078f, atom, 16);
}

// —— 左栏矢量图标（不依赖字体字形；-1 = 无图标）——
void navIcon(ImDrawList* dl, int icon, const ImVec2& c, float s, ImU32 tint) {
    const float r = s * 0.5f;
    const ImU32 white = IM_COL32(255, 255, 255, 150);
    switch (icon) {
    case 0:   // 显示：球体 + 高光
        dl->AddCircleFilled(ImVec2(c.x, c.y), r * 0.86f, tint, 28);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.26f, c.y - r * 0.30f), r * 0.26f,
                            white, 16);
        break;
    case 1:   // 分子：两球一键
        dl->AddCircleFilled(ImVec2(c.x - r * 0.52f, c.y + r * 0.30f), r * 0.42f,
                            tint, 18);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.50f, c.y - r * 0.34f), r * 0.50f,
                            tint, 18);
        dl->AddLine(ImVec2(c.x - r * 0.18f, c.y + r * 0.10f),
                    ImVec2(c.x + r * 0.16f, c.y - r * 0.10f), tint, 2.0f);
        break;
    case 2:   // 表面：包络圆 + 内芯
        dl->AddCircle(c, r * 0.88f, tint, 30, 2.0f);
        dl->AddCircleFilled(c, r * 0.24f, tint, 16);
        break;
    case 3:   // ESP：正负双色半球
        dl->AddCircleFilled(ImVec2(c.x, c.y), r * 0.82f, tint, 28);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.30f, c.y), r * 0.52f, white, 20);
        dl->AddCircle(c, r * 0.82f, tint, 28, 1.5f);
        break;
    case 4:   // 轨道：双椭圆
        dl->AddEllipse(c, ImVec2(r * 0.92f, r * 0.42f), tint, 0.85f, 26, 1.8f);
        dl->AddEllipse(c, ImVec2(r * 0.92f, r * 0.42f), tint, -0.85f, 26, 1.8f);
        dl->AddCircleFilled(c, r * 0.16f, tint, 12);
        break;
    case 5:   // 轨迹：关键帧折线 + 当前点
        dl->AddLine(ImVec2(c.x - r * 0.80f, c.y + r * 0.30f),
                    ImVec2(c.x - r * 0.18f, c.y - r * 0.22f), tint, 1.8f);
        dl->AddLine(ImVec2(c.x - r * 0.18f, c.y - r * 0.22f),
                    ImVec2(c.x + r * 0.42f, c.y + r * 0.10f), tint, 1.8f);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.80f, c.y + r * 0.30f), r * 0.16f,
                            tint, 10);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y + r * 0.10f), r * 0.16f,
                            tint, 10);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.80f, c.y - r * 0.34f), r * 0.24f,
                            tint, 14);
        break;
    case 6:   // 标注：标签框 + 锚点（水盒/标签/箭头/水印）
        dl->AddRect(ImVec2(c.x - r * 0.74f, c.y - r * 0.56f),
                    ImVec2(c.x + r * 0.16f, c.y + r * 0.10f), tint, r * 0.18f, 0, 1.8f);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.56f, c.y + r * 0.56f), r * 0.22f, tint, 14);
        dl->AddLine(ImVec2(c.x + r * 0.10f, c.y + r * 0.06f),
                    ImVec2(c.x + r * 0.40f, c.y + r * 0.42f), tint, 1.7f);
        break;
    case 7:   // 分析：柱状图
        dl->AddRectFilled(ImVec2(c.x - r * 0.72f, c.y + r * 0.10f),
                          ImVec2(c.x - r * 0.34f, c.y + r * 0.72f), tint);
        dl->AddRectFilled(ImVec2(c.x - r * 0.20f, c.y - r * 0.22f),
                          ImVec2(c.x + r * 0.18f, c.y + r * 0.72f), tint);
        dl->AddRectFilled(ImVec2(c.x + r * 0.32f, c.y - r * 0.62f),
                          ImVec2(c.x + r * 0.70f, c.y + r * 0.72f), tint);
        break;
    case 8:   // 日志：三行文本
        for (int i = 0; i < 3; ++i) {
            const float y = c.y - r * 0.52f + static_cast<float>(i) * r * 0.52f;
            const float x1 = (i == 2) ? c.x + r * 0.12f : c.x + r * 0.78f;
            dl->AddLine(ImVec2(c.x - r * 0.78f, y), ImVec2(x1, y), tint, 1.9f);
        }
        break;
    case 9:   // 设置：齿轮近似（圆 + 8 齿）
        dl->AddCircle(c, r * 0.62f, tint, 26, 2.0f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 3.14159265f * 0.25f;
            const float cx = std::cos(a);
            const float sy = std::sin(a);
            dl->AddLine(ImVec2(c.x + cx * r * 0.62f, c.y + sy * r * 0.62f),
                        ImVec2(c.x + cx * r * 0.95f, c.y + sy * r * 0.95f),
                        tint, 1.9f);
        }
        break;
    default:
        break;
    }
}

bool navButton(const char* label, bool active, const ImVec2& size, int icon) {
    const Palette& p = g;
    // 收起态 label 为空串：用图标序号做种子，避免 9 个按钮共用同一 ImGui ID
    ImGui::PushID((label != nullptr && label[0] != '\0') ? ImGui::GetID(label) : (icon + 1000));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##nav", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg = active ? col(p.selection)
                      : (hovered ? col(p.panelAlt) : IM_COL32(0, 0, 0, 0));
    // 选中态：蓝色实心**内缩胶囊**（居中、四周留出一点底色），不是整格铺满。
    // 用户反馈"被选择后中心部分应该是蓝色的"——留白让蓝色集中在视觉中心，
    // 与未选中的白卡片形成清楚层次。
    const float insetX = active ? 4.0f : 0.0f;
    const float insetY = active ? 2.0f : 0.0f;
    const ImVec2 bgMin(pos.x + insetX, pos.y + insetY);
    const ImVec2 bgMax(pos.x + size.x - insetX, pos.y + size.y - insetY);
    dl->AddRectFilled(bgMin, bgMax, bg, 9.0f);
    if (active) {
        // 白图标/白文字 + 蓝色实底；左侧再加一条半透明白条，深/浅主题都清楚。
        dl->AddRectFilled(ImVec2(bgMin.x + 3.0f, bgMin.y + (bgMax.y - bgMin.y) * 0.30f),
                          ImVec2(bgMin.x + 6.0f, bgMin.y + (bgMax.y - bgMin.y) * 0.70f),
                          IM_COL32(255, 255, 255, 190), 2.0f);
    }
    const ImU32 fg = col(active ? p.onSelection : (hovered ? p.text : p.textMuted));
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);

    if (icon >= 0 && (label == nullptr || label[0] == '\0')) {
        // 收起态：只有图标，垂直居中（文字改为 tooltip，由调用方提供）
        navIcon(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f),
                std::min(22.0f, size.x * 0.46f), fg);
    } else if (icon >= 0 && size.y >= 44.0f) {
        // 图标在上、文字在下（图标 60% 处宽）
        navIcon(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.36f),
                std::min(22.0f, size.x * 0.34f), fg);
        dl->AddText(font, fs,
                    ImVec2(pos.x + (size.x - ts.x) * 0.5f,
                           pos.y + size.y * 0.66f - ts.y * 0.5f),
                    fg, label);
    } else {
        dl->AddText(font, fs,
                    ImVec2(pos.x + (size.x - ts.x) * 0.5f,
                           pos.y + (size.y - ts.y) * 0.5f),
                    fg, label);
    }
    if (hovered) {
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    active ? IM_COL32(255, 255, 255, 130) : col(p.border), 9.0f);
    }
    ImGui::PopID();
    return clicked;
}

bool chip(const char* label, bool active, const char* tip) {
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 5));
    ImGui::PushStyleColor(ImGuiCol_Button, active ? p.selection : p.panelAlt);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          active ? mixColor(p.selection, ImVec4(1, 1, 1, 1), 0.12f)
                                 : p.accentSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          active ? p.selection : p.accentSoft);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? p.onSelection : p.text);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (tip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return clicked;
}

bool toolButton(const char* label, const char* tip) {
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(p.panel.x, p.panel.y, p.panel.z, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.accentSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.accentSoft);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (tip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return clicked;
}

// —— P0 组件（v2.7）——

bool segmented(const char* id, const char* const* items, int n, int* idx) {
    if (items == nullptr || idx == nullptr || n <= 0) {
        return false;
    }
    const Palette& p = g;
    ImGui::PushID(id);
    int cur = (*idx >= 0 && *idx < n) ? *idx : 0;
    const float avail = std::max(ImGui::GetContentRegionAvail().x, 80.0f);
    const float h = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail, origin.y + h),
                      col(p.panelSunken), h * 0.5f);

    const float inset = 2.0f;
    const float gap = 3.0f;
    const float each = (avail - inset * 2.0f - gap * static_cast<float>(n - 1)) /
                       static_cast<float>(n);
    bool changed = false;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, (h - inset * 2.0f) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + inset, origin.y + inset));
    for (int i = 0; i < n; ++i) {
        const bool sel = (i == cur);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              sel ? p.selection : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              sel ? mixColor(p.selection, ImVec4(1, 1, 1, 1), 0.12f)
                                  : p.accentSoft);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              sel ? p.selection : p.accentSoft);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? p.onSelection : p.textMuted);
        if (i > 0) {
            ImGui::SameLine(0.0f, gap);
        }
        if (ImGui::Button(items[i], ImVec2(each, h - inset * 2.0f))) {
            if (cur != i) {
                cur = i;
                *idx = i;
                changed = true;
            }
        }
        ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + 2.0f));
    ImGui::PopID();
    return changed;
}

bool sliderNum(const char* label, float* value, float vmin, float vmax,
               const char* fmt, float step, const char* tip) {
    if (value == nullptr) {
        return false;
    }
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::TextColored(p.textMuted, "%s", label);
    if (tip != nullptr) {
        helpTip(tip);
    }
    const float boxW = 62.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    bool changed = false;
    ImGui::SetNextItemWidth(std::max(avail - boxW - 6.0f, 40.0f));
    changed |= ImGui::SliderFloat("##s", value, vmin, vmax, "");
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(boxW);
    changed |= ImGui::DragFloat("##n", value, step, vmin, vmax, fmt,
                                ImGuiSliderFlags_AlwaysClamp);
    if (*value < vmin) {
        *value = vmin;
        changed = true;
    } else if (*value > vmax) {
        *value = vmax;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool stepper(const char* label, int* value, int step, int lo, int hi,
             const char* tip) {
    if (value == nullptr) {
        return false;
    }
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::TextColored(p.textMuted, "%s", label);
    if (tip != nullptr) {
        helpTip(tip);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    bool changed = false;
    if (ImGui::Button("-##m", ImVec2(26.0f, 0.0f))) {
        *value = std::max(lo, *value - step);
        changed = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::SetNextItemWidth(52.0f);
    int v = *value;
    if (ImGui::DragInt("##v", &v, 1.0f, lo, hi)) {
        *value = std::clamp(v, lo, hi);
        changed = true;
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button("+##p", ImVec2(26.0f, 0.0f))) {
        *value = std::min(hi, *value + step);
        changed = true;
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

void badge(const char* text, BadgeKind kind) {
    if (text == nullptr) {
        return;
    }
    const Palette& p = g;
    ImVec4 base = p.textMuted;
    switch (kind) {
        case BadgeKind::Info: base = p.accent; break;
        case BadgeKind::Ok: base = p.ok; break;
        case BadgeKind::Warn: base = p.warn; break;
        case BadgeKind::Danger: base = p.danger; break;
        case BadgeKind::Neutral:
        default: base = p.textMuted; break;
    }
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(8.0f, 2.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      col(ImVec4(base.x, base.y, base.z, 0.16f)), r);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                col(ImVec4(base.x, base.y, base.z, 0.55f)), r);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), col(base), text);
    ImGui::Dummy(size);
}

void statChip(const char* label, const char* value) {
    if (label == nullptr || value == nullptr) {
        return;
    }
    const Palette& p = g;
    const ImVec2 ls = ImGui::CalcTextSize(label);
    const ImVec2 vs = ImGui::CalcTextSize(value);
    const ImVec2 pad(9.0f, 4.0f);
    const float gap = 6.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ls.x + vs.x + gap + pad.x * 2.0f,
                      std::max(ls.y, vs.y) + pad.y * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col(p.panelAlt),
                      6.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), col(p.textMuted), label);
    dl->AddText(ImVec2(pos.x + pad.x + ls.x + gap, pos.y + pad.y), col(p.text),
                value);
    ImGui::Dummy(size);
}void helpTip(const char* tip) {
    const Palette& p = g;
    // v2.7：悬停标签本身或「(?)」都弹出提示
    const bool labelHovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    ImGui::TextColored(p.textFaint, "(?)");
    if (labelHovered || ImGui::IsItemHovered()) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, g.panel);
        ImGui::SetTooltip("%s", tip);
        ImGui::PopStyleColor();
    }
}

bool sliderRow(const char* label, float* value, float vmin, float vmax,
               const char* fmt, const char* tip) {
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::TextColored(p.textMuted, "%s", label);
    if (tip != nullptr) {
        helpTip(tip);
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::SliderFloat("##v", value, vmin, vmax, fmt);
    ImGui::PopID();
    return changed;
}

bool toggleRow(const char* label, bool* value, const char* tip) {
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    const bool changed = ImGui::Checkbox(label, value);
    ImGui::PopStyleColor();
    if (tip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return changed;
}

bool comboRow(const char* label, int* index, const char* items, const char* tip) {
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::TextColored(p.textMuted, "%s", label);
    if (tip != nullptr) {
        helpTip(tip);
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::Combo("##c", index, items);
    ImGui::PopID();
    return changed;
}

bool inputRow(const char* label, char* buf, std::size_t size, const char* hint) {
    const Palette& p = g;
    ImGui::PushID(label);
    ImGui::TextColored(p.textMuted, "%s", label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::InputTextWithHint(
        "##i", hint != nullptr ? hint : "", buf, size);
    ImGui::PopID();
    return changed;
}

void kvRow(const char* key, const char* value) {
    const Palette& p = g;
    ImGui::TextColored(p.textMuted, "%s", key);
    const float w = ImGui::GetContentRegionAvail().x;
    const float vw = ImGui::CalcTextSize(value).x;
    ImGui::SameLine(w - vw - 4.0f);
    ImGui::TextColored(p.text, "%s", value);
}

void spacer(float h) { ImGui::Dummy(ImVec2(0, h)); }

void hline() {
    ImGui::PushStyleColor(ImGuiCol_Separator, g.border);
    ImGui::Separator();
    ImGui::PopStyleColor();
}

} // namespace moleculens::app::aurora
