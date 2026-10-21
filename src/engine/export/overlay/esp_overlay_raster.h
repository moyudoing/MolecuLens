// =============================================================================
// MolecuLens · engine · export/overlay/esp_overlay_raster.h —— ESP 色标条/极值标签栅格化
// 用途：把 ESP 表面的色标条与极值点数值叠加到 RGBA8 像素缓冲上（CLI 无头出图、
//       GUI 导出 PNG 共用；数据来自 engine::espOverlaySnapshot）。
// 文字：优先用 text_raster_ttf（Noto Sans SC 子集，抗锯齿+描边，清晰）；字体缺失时回退
//       内置 5x7 位图字体（保证功能可用）。布局参考 GXNU MolStudio 截图：
//       右侧竖色标条 + 6 刻度 + 标题，表面极值点圆点 + 引导线 + 数值。
// =============================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "engine/export/overlay/text_raster_ttf.h"
#include "engine/style/espsurface/esp_surface.h"

namespace moleculens::engine::esp_overlay {

// ---------------------------------------------------------------------------
// 基础绘制工具
// ---------------------------------------------------------------------------

inline void putPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y, int r, int g,
                     int b, float alpha = 1.0f) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 4;
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    rgba[o + 0] = static_cast<uint8_t>(rgba[o + 0] * (1.0f - a) + r * a + 0.5f);
    rgba[o + 1] = static_cast<uint8_t>(rgba[o + 1] * (1.0f - a) + g * a + 0.5f);
    rgba[o + 2] = static_cast<uint8_t>(rgba[o + 2] * (1.0f - a) + b * a + 0.5f);
}

inline void fillRect(std::vector<uint8_t>& rgba, int w, int h, int x, int y, int rw, int rh,
                     int r, int g, int b, float a = 1.0f) {
    for (int yy = y; yy < y + rh; ++yy) {
        for (int xx = x; xx < x + rw; ++xx) {
            putPixel(rgba, w, h, xx, yy, r, g, b, a);
        }
    }
}

inline void drawDisc(std::vector<uint8_t>& rgba, int w, int h, int cx, int cy, int radius,
                     int r, int g, int b, float a = 1.0f) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy > radius * radius) {
                continue;
            }
            putPixel(rgba, w, h, cx + dx, cy + dy, r, g, b, a);
        }
    }
}

inline void drawLine(std::vector<uint8_t>& rgba, int w, int h, int x0, int y0, int x1,
                     int y1, int r, int g, int b, float a = 1.0f, int thickness = 1) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int steps = std::max(dx, dy);
    if (steps == 0) {
        putPixel(rgba, w, h, x0, y0, r, g, b, a);
        return;
    }
    const int half = std::max(0, thickness / 2);
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t));
        const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
        for (int oy = -half; oy <= half; ++oy) {
            for (int ox = -half; ox <= half; ++ox) {
                putPixel(rgba, w, h, x + ox, y + oy, r, g, b, a);
            }
        }
    }
}

// 世界坐标 → 像素坐标；返回 false 表示在相机背后/裁剪
inline bool projectToPixel(const glm::mat4& viewProj, const glm::vec3& world, int w, int h,
                           float& outX, float& outY) {
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    if (!(clip.w > 1e-6f)) {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.z < -1.0f || ndc.z > 1.0f) {
        return false;
    }
    if (ndc.x < -1.2f || ndc.x > 1.2f || ndc.y < -1.2f || ndc.y > 1.2f) {
        return false;
    }
    outX = (ndc.x * 0.5f + 0.5f) * static_cast<float>(w);
    outY = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(h);
    return true;
}

// 值 → 色标 t（invert 由调用方处理）
inline float valueToT(float v, float vmin, float vmax) {
    if (vmax - vmin < 1e-9f) {
        return 0.5f;
    }
    return std::clamp((v - vmin) / (vmax - vmin), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// 文字绘制（TTF 优先；字体缺失回退 5x7 位图）
// ---------------------------------------------------------------------------

namespace detail {

struct TextMetrics {
    int title = 22;   // 色标标题
    int tick = 15;    // 刻度数值
    int label = 20;   // 极值数值
};

inline TextMetrics metricsFor(int w, int h) {
    TextMetrics m;
    m.title = std::max(15, static_cast<int>(std::lround(h * 0.022)));
    m.tick = std::max(11, static_cast<int>(std::lround(h * 0.014)));
    m.label = std::max(13, static_cast<int>(std::lround(h * 0.019)));
    (void)w;
    return m;
}

// —— 5x7 位图字体（回退用）——
struct Glyph {
    uint8_t rows[7];
};

inline const Glyph* glyphFor(char c) {
    static const Glyph kDigits[10] = {
        {{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},  // 0
        {{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},  // 1
        {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},  // 2
        {{0x1F, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1F}},  // 3
        {{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},  // 4
        {{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},  // 5
        {{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},  // 6
        {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},  // 7
        {{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},  // 8
        {{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},  // 9
    };
    static const Glyph kDot = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}};
    static const Glyph kMinus = {{0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}};
    static const Glyph kPlus = {{0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}};
    static const Glyph kE = {{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}};
    static const Glyph kS = {{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}};
    static const Glyph kP = {{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}};
    static const Glyph kK = {{0x10, 0x10, 0x14, 0x18, 0x14, 0x12, 0x11}};
    static const Glyph kC = {{0x00, 0x0E, 0x10, 0x10, 0x10, 0x11, 0x0E}};
    static const Glyph kA = {{0x00, 0x0E, 0x01, 0x0F, 0x11, 0x13, 0x0D}};
    static const Glyph kL = {{0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}};
    static const Glyph kM = {{0x00, 0x1A, 0x15, 0x15, 0x15, 0x15, 0x15}};
    static const Glyph kO = {{0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E}};
    static const Glyph kSlash = {{0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}};
    static const Glyph kLParen = {{0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}};
    static const Glyph kRParen = {{0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}};
    static const Glyph kSpace = {{0, 0, 0, 0, 0, 0, 0}};
    if (c >= '0' && c <= '9') {
        return &kDigits[c - '0'];
    }
    switch (c) {
        case '.': return &kDot;
        case '-': return &kMinus;
        case '+': return &kPlus;
        case 'E': return &kE;
        case 'S': return &kS;
        case 'P': return &kP;
        case 'k': return &kK;
        case 'c': return &kC;
        case 'a': return &kA;
        case 'l': return &kL;
        case 'm': return &kM;
        case 'o': return &kO;
        case '/': return &kSlash;
        case '(': return &kLParen;
        case ')': return &kRParen;
        default: return &kSpace;
    }
}

inline float fallbackMeasure(const char* text, int px) {
    const float scale = std::max(1.0f, static_cast<float>(px) / 7.0f);
    float n = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        n += (*p == ' ') ? 3.0f : 6.0f;
    }
    return n * scale;
}

inline float fallbackDraw(std::vector<uint8_t>& rgba, int w, int h, float x, float baselineY,
                          const char* text, int px, uint8_t r, uint8_t g, uint8_t b,
                          uint8_t a, int outline, uint8_t orr, uint8_t og, uint8_t ob) {
    const float scale = std::max(1.0f, static_cast<float>(px) / 7.0f);
    const int glyphH = static_cast<int>(std::lround(7.0f * scale));
    const int top = static_cast<int>(std::lround(baselineY)) - glyphH;
    const auto drawOnce = [&](int ox, int oy, uint8_t cr, uint8_t cg, uint8_t cb,
                              uint8_t ca) {
        float penX = x + static_cast<float>(ox);
        for (const char* p = text; *p != '\0'; ++p) {
            if (*p == ' ') {
                penX += 3.0f * scale;
                continue;
            }
            const Glyph* gl = glyphFor(*p);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if ((gl->rows[row] & (1u << (4 - col))) == 0) {
                        continue;
                    }
                    const int px0 = static_cast<int>(std::lround(penX)) +
                                    static_cast<int>(std::lround(col * scale));
                    const int py0 = top + oy + static_cast<int>(std::lround(row * scale));
                    const int side = std::max(1, static_cast<int>(std::lround(scale)));
                    fillRect(rgba, w, h, px0, py0, side, side, cr, cg, cb,
                             static_cast<float>(ca) / 255.0f);
                }
            }
            penX += 6.0f * scale;
        }
    };
    if (outline > 0) {
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) {
                    continue;
                }
                drawOnce(ox, oy, orr, og, ob, static_cast<uint8_t>(std::min<int>(a, 200)));
            }
        }
    }
    drawOnce(0, 0, r, g, b, a);
    return fallbackMeasure(text, px);
}

inline float measureText(const char* text, int px) {
    if (const text::TtfRenderer* font = text::defaultRenderer(); font != nullptr) {
        return font->measure(text, px);
    }
    return fallbackMeasure(text, px);
}

// 以 (x, baselineY) 为基线绘制一行文字；返回 advance 宽度
inline float drawText(std::vector<uint8_t>& rgba, int w, int h, float x, float baselineY,
                      const char* text, int px, uint8_t r, uint8_t g, uint8_t b,
                      uint8_t a, int outline, uint8_t orr, uint8_t og, uint8_t ob) {
    if (const text::TtfRenderer* font = text::defaultRenderer(); font != nullptr) {
        return font->draw(rgba, w, h, x, baselineY, text, px, r, g, b, a, outline, orr,
                          og, ob);
    }
    return fallbackDraw(rgba, w, h, x, baselineY, text, px, r, g, b, a, outline, orr, og,
                        ob);
}

} // namespace detail

// ---------------------------------------------------------------------------
// 主入口：把 ESP 色标条 + 极值点数值叠加到 RGBA8 缓冲
// ---------------------------------------------------------------------------

// darkBackground 用于选文字/描边配色（暗底白字黑边 / 亮底深字白边）。
inline void drawEspOverlay(std::vector<uint8_t>& rgba, int w, int h,
                           const glm::mat4& viewProj, const EspOverlay& ov,
                           bool darkBackground = true, bool drawBar = true,
                           bool drawLabels = true) {
    if (!ov.active || rgba.size() < static_cast<size_t>(w) * h * 4) {
        return;
    }
    const detail::TextMetrics tm = detail::metricsFor(w, h);
    const uint8_t tr = darkBackground ? 242 : 22;
    const uint8_t tg = darkBackground ? 242 : 22;
    const uint8_t tb = darkBackground ? 246 : 26;
    const uint8_t orr = darkBackground ? 0 : 255;
    const uint8_t og = darkBackground ? 0 : 255;
    const uint8_t ob = darkBackground ? 0 : 255;
    const uint8_t labelAlpha = 255;

    // —— 色标条（右侧竖条）——
    if (drawBar && ov.vmaxKcal - ov.vminKcal > 1e-6f) {
        const int barW = std::max(14, static_cast<int>(std::lround(w * 0.012)));
        const int barH = static_cast<int>(std::lround(h * 0.52));
        const int barX = w - static_cast<int>(std::lround(w * 0.10));
        const int barY = (h - barH) / 2;
        const int outlineWidth = std::max(1, tm.title / 16);

        // 渐变（top = vmax）
        for (int i = 0; i < barH; ++i) {
            const float frac = static_cast<float>(i) / static_cast<float>(barH - 1);
            const float value = ov.vmaxKcal + (ov.vminKcal - ov.vmaxKcal) * frac;
            float t = valueToT(value, ov.vminKcal, ov.vmaxKcal);
            if (ov.invert) {
                t = 1.0f - t;
            }
            const glm::vec3 c = espColormapColor(ov.colormap, t);
            fillRect(rgba, w, h, barX, barY + i, barW, 1,
                     static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f),
                     static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                     static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
        }
        // 外框
        fillRect(rgba, w, h, barX - 1, barY - 1, barW + 2, 1, tr, tg, tb, 0.85f);
        fillRect(rgba, w, h, barX - 1, barY + barH, barW + 2, 1, tr, tg, tb, 0.85f);
        fillRect(rgba, w, h, barX - 1, barY, 1, barH, tr, tg, tb, 0.85f);
        fillRect(rgba, w, h, barX + barW, barY, 1, barH, tr, tg, tb, 0.85f);

        // 标题（底部对齐色标条顶部上方）
        const char* title = "ESP (kcal/mol)";
        const float titleW = detail::measureText(title, tm.title);
        const float titleBaseline = static_cast<float>(barY) - tm.title * 0.42f;
        detail::drawText(rgba, w, h,
                         static_cast<float>(barX + barW / 2) - titleW * 0.5f,
                         titleBaseline, title, tm.title, tr, tg, tb, labelAlpha,
                         outlineWidth, orr, og, ob);

        // 刻度：6 个等间距值（上 = vmax，右对齐到色标条左侧）
        const int ticks = 6;
        char buf[32];
        const int gap = std::max(6, tm.tick / 2);
        for (int i = 0; i < ticks; ++i) {
            const float frac = static_cast<float>(i) / static_cast<float>(ticks - 1);
            const float value = ov.vmaxKcal + (ov.vminKcal - ov.vmaxKcal) * frac;
            const int y = barY + static_cast<int>(frac * static_cast<float>(barH - 1));
            fillRect(rgba, w, h, barX - gap, y, gap, std::max(1, outlineWidth + 1), tr,
                     tg, tb, 0.85f);
            std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(value));
            const float tw = detail::measureText(buf, tm.tick);
            detail::drawText(rgba, w, h,
                             static_cast<float>(barX - gap - 2) - tw,
                             static_cast<float>(y) + tm.tick * 0.35f, buf, tm.tick, tr,
                             tg, tb, labelAlpha, outlineWidth, orr, og, ob);
        }
    }

    // —— 极值点标签（按 |值| 降序放置；屏幕空间去重叠）——
    if (drawLabels && !ov.extrema.empty()) {
        const int px = tm.label;
        const int dotRadius = std::max(3, static_cast<int>(std::lround(h * 0.0035)));
        const int outlineWidth = std::max(1, px / 16);
        const int minGap = static_cast<int>(std::lround(px * 2.2f));
        std::vector<const EspExtremum*> ordered;
        ordered.reserve(ov.extrema.size());
        for (const auto& ex : ov.extrema) {
            ordered.push_back(&ex);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return std::fabs(a->valueKcal) > std::fabs(b->valueKcal);
        });
        std::vector<std::pair<int, int>> placed;
        for (const EspExtremum* ex : ordered) {
            float fx = 0.0f;
            float fy = 0.0f;
            if (!projectToPixel(viewProj, ex->position, w, h, fx, fy)) {
                continue;
            }
            const int cx = static_cast<int>(std::lround(fx));
            const int cy = static_cast<int>(std::lround(fy));
            bool tooClose = false;
            for (const auto& p : placed) {
                if (std::abs(p.first - cx) < minGap && std::abs(p.second - cy) < minGap) {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) {
                continue;
            }
            placed.emplace_back(cx, cy);

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(ex->valueKcal));
            const float textW = detail::measureText(buf, px);
            const int leader = std::max(10, static_cast<int>(std::lround(px * 0.8f)));
            const float baselineY = static_cast<float>(cy) + px * 0.35f;
            float textX = static_cast<float>(cx + dotRadius + leader);
            int dir = 1;
            if (textX + textW > static_cast<float>(w - 8)) {
                textX = static_cast<float>(cx - dotRadius - leader) - textW;
                dir = -1;
            }
            const int lineEndX = static_cast<int>(std::lround(
                textX + (dir > 0 ? 0.0f : textW) + (dir > 0 ? -4.0f : 4.0f)));

            // 引导线（先画，圆点覆盖在其上）
            drawLine(rgba, w, h, cx + dir * dotRadius, cy, lineEndX, cy, tr, tg, tb, 0.75f,
                     std::max(1, outlineWidth));
            // 圆点：极大金色 / 极小浅蓝，深色描边
            const int rr = ex->isMaximum ? 255 : 150;
            const int gg = ex->isMaximum ? 198 : 205;
            const int bb = ex->isMaximum ? 54 : 255;
            drawDisc(rgba, w, h, cx, cy, dotRadius + std::max(1, outlineWidth),
                     orr, og, ob, 0.85f);
            drawDisc(rgba, w, h, cx, cy, dotRadius, rr, gg, bb, 1.0f);
            detail::drawText(rgba, w, h, textX, baselineY, buf, px, tr, tg, tb, labelAlpha,
                             outlineWidth, orr, og, ob);
        }
    }
}

} // namespace moleculens::engine::esp_overlay
