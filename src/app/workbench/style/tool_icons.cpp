// =============================================================================
// Moleculens · app/workbench/style/tool_icons.cpp —— 左侧工具栏图标（v2.17）
//
// 为什么单独一份文件：Aurora 的 navIcon 是"工作区导航"的图标，这里是"工具"图标
// （指针/尺子/标签/箭头/取景/坐标轴…），两者语义不同、也不共用序号。放在一起会
// 让 navIcon 的 switch 与本文件的 switch 相互干扰（同一个 int 各自解释）。
//
// 全部用 ImDrawList 的图元现画：
//   · 不引图标字体（项目已裁过中文字体子集，再加一套图标字体要多拷一个 ttf）；
//   · 不依赖位图资源，高 DPI / 任意缩放下都是矢量边缘；
//   · 颜色由调用方给（选中=白、hover=正文色、关=次要色），浅色/深色主题自动跟随。
// =============================================================================

#include "app/workbench/style/aurora.h"

#include <algorithm>
#include <cmath>

namespace moleculens::app::aurora {

namespace {

constexpr float kPi = 3.14159265358979f;

// 极坐标取点
ImVec2 polar(const ImVec2& c, float radius, float angle) {
    return ImVec2(c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius);
}

} // namespace

void toolIcon(ImDrawList* dl, ToolIcon icon, const ImVec2& c, float r, ImU32 tint) {
    if (dl == nullptr || r <= 0.5f) {
        return;
    }
    // 线宽随图标半径缩放：11 px 图标 ≈ 1.8 px 线宽，与 14 px 文字的笔画接近。
    const float t = std::max(1.2f, r * 0.16f);

    switch (icon) {
        case ToolIcon::Pointer: {
            // 光标：实心箭头 + 尾巴，形状来自系统指针（一眼可辨"默认/浏览"）
            const ImVec2 a(c.x - r * 0.52f, c.y - r * 0.88f);
            const ImVec2 b(c.x + r * 0.64f, c.y - r * 0.02f);
            const ImVec2 d(c.x - r * 0.10f, c.y + r * 0.10f);
            dl->AddTriangleFilled(a, b, d, tint);
            dl->AddLine(d, ImVec2(c.x - r * 0.34f, c.y + r * 0.88f), tint, t * 1.7f);
            break;
        }
        case ToolIcon::Ruler: {
            // 斜置直尺：矩形轮廓 + 三道刻度
            const float ang = -0.72f;
            const ImVec2 u(std::cos(ang), std::sin(ang));
            const ImVec2 v(-u.y, u.x);
            const float hl = r * 0.92f;
            const float hw = r * 0.30f;
            auto at = [&](float s, float w) {
                return ImVec2(c.x + u.x * s * hl + v.x * w * hw,
                              c.y + u.y * s * hl + v.y * w * hw);
            };
            dl->AddQuad(at(-1, -1), at(1, -1), at(1, 1), at(-1, 1), tint, t);
            for (int i = -1; i <= 1; ++i) {
                const float s = static_cast<float>(i) * 0.5f;
                dl->AddLine(at(s, -1), at(s, -0.12f), tint, t * 0.85f);
            }
            break;
        }
        case ToolIcon::TextLabel: {
            // 大写字形 "T"：标注 = 文字
            dl->AddLine(ImVec2(c.x - r * 0.64f, c.y - r * 0.56f),
                        ImVec2(c.x + r * 0.64f, c.y - r * 0.56f), tint, t * 1.25f);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.56f), ImVec2(c.x, c.y + r * 0.74f), tint,
                        t * 1.25f);
            break;
        }
        case ToolIcon::Arrow: {
            const ImVec2 p0(c.x - r * 0.74f, c.y + r * 0.66f);
            const ImVec2 p1(c.x + r * 0.62f, c.y - r * 0.54f);
            dl->AddLine(p0, p1, tint, t * 1.15f);
            ImVec2 dir(p1.x - p0.x, p1.y - p0.y);
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len > 0.001f) {
                dir.x /= len;
                dir.y /= len;
                const ImVec2 n(-dir.y, dir.x);
                const ImVec2 tip(p1.x + dir.x * r * 0.16f, p1.y + dir.y * r * 0.16f);
                const ImVec2 base(p1.x - dir.x * r * 0.34f, p1.y - dir.y * r * 0.34f);
                dl->AddTriangleFilled(
                    tip, ImVec2(base.x + n.x * r * 0.38f, base.y + n.y * r * 0.38f),
                    ImVec2(base.x - n.x * r * 0.38f, base.y - n.y * r * 0.38f), tint);
            }
            break;
        }
        case ToolIcon::Frame: {
            // 四角取景框（截图/F 键的通用语义）
            const float x0 = c.x - r * 0.80f;
            const float x1 = c.x + r * 0.80f;
            const float y0 = c.y - r * 0.80f;
            const float y1 = c.y + r * 0.80f;
            const float l = r * 0.52f;
            dl->AddLine(ImVec2(x0, y0), ImVec2(x0 + l, y0), tint, t);
            dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y0 + l), tint, t);
            dl->AddLine(ImVec2(x1, y0), ImVec2(x1 - l, y0), tint, t);
            dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y0 + l), tint, t);
            dl->AddLine(ImVec2(x0, y1), ImVec2(x0 + l, y1), tint, t);
            dl->AddLine(ImVec2(x0, y1), ImVec2(x0, y1 - l), tint, t);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x1 - l, y1), tint, t);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x1, y1 - l), tint, t);
            break;
        }
        case ToolIcon::Reset: {
            // 开口圆环 + 箭头：撤销/复位
            dl->PathArcTo(c, r * 0.66f, 0.95f, 5.15f, 28);
            dl->PathStroke(tint, 0, t);
            const float a0 = 0.95f;
            const ImVec2 hp = polar(c, r * 0.66f, a0);
            const ImVec2 tg(-std::sin(a0), std::cos(a0));
            const ImVec2 nr(std::cos(a0), std::sin(a0));
            dl->AddTriangleFilled(
                ImVec2(hp.x + tg.x * r * 0.22f, hp.y + tg.y * r * 0.22f),
                ImVec2(hp.x - tg.x * r * 0.26f + nr.x * r * 0.24f,
                       hp.y - tg.y * r * 0.26f + nr.y * r * 0.24f),
                ImVec2(hp.x - tg.x * r * 0.26f - nr.x * r * 0.24f,
                       hp.y - tg.y * r * 0.26f - nr.y * r * 0.24f),
                tint);
            break;
        }
        case ToolIcon::Axes: {
            const ImVec2 o(c.x - r * 0.64f, c.y + r * 0.64f);
            const ImVec2 ex(c.x + r * 0.80f, o.y);
            const ImVec2 ey(o.x, c.y - r * 0.82f);
            const ImVec2 ez(c.x + r * 0.16f, c.y + r * 0.12f);
            dl->AddLine(o, ex, tint, t);
            dl->AddLine(o, ey, tint, t);
            dl->AddLine(o, ez, tint, t * 0.9f);
            dl->AddTriangleFilled(ex, ImVec2(ex.x - r * 0.22f, o.y - r * 0.17f),
                                  ImVec2(ex.x - r * 0.22f, o.y + r * 0.17f), tint);
            dl->AddTriangleFilled(ey, ImVec2(o.x - r * 0.17f, ey.y + r * 0.22f),
                                  ImVec2(o.x + r * 0.17f, ey.y + r * 0.22f), tint);
            dl->AddCircleFilled(o, r * 0.12f, tint, 10);
            break;
        }
        case ToolIcon::ScaleBar: {
            const float y = c.y + r * 0.12f;
            const float x0 = c.x - r * 0.84f;
            const float x1 = c.x + r * 0.84f;
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), tint, t);
            dl->AddLine(ImVec2(x0, y - r * 0.36f), ImVec2(x0, y + r * 0.36f), tint, t);
            dl->AddLine(ImVec2(x1, y - r * 0.36f), ImVec2(x1, y + r * 0.36f), tint, t);
            dl->AddLine(ImVec2(c.x, y - r * 0.22f), ImVec2(c.x, y + r * 0.22f), tint,
                        t * 0.8f);
            break;
        }
        case ToolIcon::Box: {
            // 等轴测立方体：顶面菱形 + 三条竖棱 + 底面两条棱
            const ImVec2 top(c.x, c.y - r * 0.92f);
            const ImVec2 right(c.x + r * 0.82f, c.y - r * 0.46f);
            const ImVec2 mid(c.x, c.y + r * 0.0f);
            const ImVec2 left(c.x - r * 0.82f, c.y - r * 0.46f);
            const float h = r * 0.86f;
            dl->AddQuad(top, right, mid, left, tint, t);
            const ImVec2 bl(left.x, left.y + h);
            const ImVec2 bm(mid.x, mid.y + h);
            const ImVec2 br(right.x, right.y + h);
            dl->AddLine(left, bl, tint, t);
            dl->AddLine(mid, bm, tint, t);
            dl->AddLine(right, br, tint, t);
            dl->AddLine(bl, bm, tint, t);
            dl->AddLine(bm, br, tint, t);
            break;
        }
        case ToolIcon::Hydrogen: {
            // 字母 "H"：隐藏氢原子
            dl->AddLine(ImVec2(c.x - r * 0.60f, c.y - r * 0.66f),
                        ImVec2(c.x - r * 0.60f, c.y + r * 0.66f), tint, t);
            dl->AddLine(ImVec2(c.x + r * 0.60f, c.y - r * 0.66f),
                        ImVec2(c.x + r * 0.60f, c.y + r * 0.66f), tint, t);
            dl->AddLine(ImVec2(c.x - r * 0.60f, c.y), ImVec2(c.x + r * 0.60f, c.y), tint, t);
            break;
        }
        case ToolIcon::Camera: {
            dl->AddRect(ImVec2(c.x - r * 0.84f, c.y - r * 0.38f),
                        ImVec2(c.x + r * 0.84f, c.y + r * 0.58f), tint, r * 0.22f, 0, t);
            dl->AddLine(ImVec2(c.x - r * 0.32f, c.y - r * 0.38f),
                        ImVec2(c.x - r * 0.32f, c.y - r * 0.62f), tint, t);
            dl->AddLine(ImVec2(c.x - r * 0.32f, c.y - r * 0.62f),
                        ImVec2(c.x + r * 0.06f, c.y - r * 0.62f), tint, t);
            dl->AddLine(ImVec2(c.x + r * 0.06f, c.y - r * 0.62f),
                        ImVec2(c.x + r * 0.06f, c.y - r * 0.38f), tint, t);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.10f), r * 0.26f, tint, 16);
            break;
        }
        case ToolIcon::Sun: {
            // 光源：圆 + 八条射线（光追预览）
            dl->AddCircleFilled(c, r * 0.34f, tint, 16);
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * kPi * 0.25f;
                dl->AddLine(polar(c, r * 0.58f, a), polar(c, r * 0.92f, a), tint, t);
            }
            break;
        }
        case ToolIcon::Theme: {
            // 半明半暗圆：浅色 / 深色切换
            dl->AddCircle(c, r * 0.76f, tint, 28, t);
            dl->PathArcTo(c, r * 0.72f, -kPi * 0.5f, kPi * 0.5f, 20);
            dl->PathFillConvex(tint);
            break;
        }
        case ToolIcon::Trash: {
            // 垃圾桶：清除测量结果
            dl->AddLine(ImVec2(c.x - r * 0.66f, c.y - r * 0.54f),
                        ImVec2(c.x + r * 0.66f, c.y - r * 0.54f), tint, t);
            dl->AddLine(ImVec2(c.x - r * 0.24f, c.y - r * 0.54f),
                        ImVec2(c.x - r * 0.14f, c.y - r * 0.80f), tint, t * 0.9f);
            dl->AddLine(ImVec2(c.x - r * 0.14f, c.y - r * 0.80f),
                        ImVec2(c.x + r * 0.14f, c.y - r * 0.80f), tint, t * 0.9f);
            dl->AddLine(ImVec2(c.x + r * 0.14f, c.y - r * 0.80f),
                        ImVec2(c.x + r * 0.24f, c.y - r * 0.54f), tint, t * 0.9f);
            dl->AddQuad(ImVec2(c.x - r * 0.48f, c.y - r * 0.34f),
                        ImVec2(c.x + r * 0.48f, c.y - r * 0.34f),
                        ImVec2(c.x + r * 0.36f, c.y + r * 0.84f),
                        ImVec2(c.x - r * 0.36f, c.y + r * 0.84f), tint, t);
            dl->AddLine(ImVec2(c.x - r * 0.16f, c.y - r * 0.18f),
                        ImVec2(c.x - r * 0.10f, c.y + r * 0.64f), tint, t * 0.75f);
            dl->AddLine(ImVec2(c.x + r * 0.16f, c.y - r * 0.18f),
                        ImVec2(c.x + r * 0.10f, c.y + r * 0.64f), tint, t * 0.75f);
            break;
        }
        case ToolIcon::Count:
        default:
            dl->AddCircleFilled(c, r * 0.4f, tint, 12);
            break;
    }
}

bool shelfButton(ToolIcon icon, bool active, bool toggle, const ImVec2& size,
                 const char* tip) {
    const Palette& p = palette();
    ImGui::PushID(static_cast<int>(icon));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const bool clicked = ImGui::InvisibleButton("##shelf_btn", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ⚠ 开关态不要用 accent / accentSoft 上色：主色可被用户改成很浅的粉，
    // 「淡底 + 主色图标」会变成"粉底粉图标"（实测在浅色主题下几乎看不见）。
    // 因此开关态改用**固定选中蓝**（Palette::selection，与 v2.14 的导航/列表项
    // 同一决策）的淡底 + 实色图标；互斥模式仍是选中蓝实底 + 白图标。
    const ImVec4 tint(p.selection.x, p.selection.y, p.selection.z, 0.14f);
    const ImVec4 tintHover(p.selection.x, p.selection.y, p.selection.z, 0.22f);

    if (active && !toggle) {
        dl->AddRectFilled(pos, max, col(p.selection), 9.0f);
        // 左侧白条：与 navButton 的选中态同一语言
        dl->AddRectFilled(ImVec2(pos.x + 3.0f, pos.y + size.y * 0.28f),
                          ImVec2(pos.x + 6.0f, pos.y + size.y * 0.72f),
                          IM_COL32(255, 255, 255, 190), 2.0f);
    } else if (active) {
        dl->AddRectFilled(pos, max, col(hovered ? tintHover : tint), 9.0f);
        dl->AddRect(pos, max, col(p.selection), 9.0f, 0, 1.2f);
    } else if (hovered) {
        dl->AddRectFilled(pos, max, col(tint), 9.0f);
    }

    const ImU32 fg = (active && !toggle)
                         ? col(p.onSelection)
                         : (active ? col(p.selection) : (hovered ? col(p.text) : col(p.textMuted)));
    toolIcon(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f),
             std::min(size.x, size.y) * 0.27f, fg);

    if (hovered) {
        dl->AddRect(pos, max, (active && !toggle) ? IM_COL32(255, 255, 255, 130)
                                                  : col(p.borderStrong),
                    9.0f);
    }
    if (tip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return clicked;
}

} // namespace moleculens::app::aurora
