// =============================================================================
// MolecuLens · engine · export/overlay/annotation/scene_overlay_raster.h
// —— 场景标注（水盒 / 标签 / 箭头 / 水印）的软件栅格化
//
// 为什么复用 esp_overlay_raster：那里已经有 putPixel / drawLine / drawDisc /
// projectToPixel 与 TTF 文字（含 5x7 回退字体），标注只需要三角形填充与语义封装。
// 这样 PNG 导出与视口叠加用**同一套几何与配色**，导出的图不会比屏幕少东西。
//
// 坐标约定：viewProj 的宽高比必须与目标图像一致（调用方保证）；文字像素尺寸
// 以 900 px 高的视口为基准等比放大（label.pixelSize=16 → 1080p 约 19 px）。
// =============================================================================

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/export/overlay/annotation/scene_overlay.h"
#include "engine/export/overlay/esp_overlay_raster.h"

namespace moleculens::engine::scene_overlay {

namespace detail {

inline void rgbaToBytes(const float c[4], int* r, int* g, int* b, float* a) {
    *r = static_cast<int>(std::clamp(c[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    *g = static_cast<int>(std::clamp(c[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    *b = static_cast<int>(std::clamp(c[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    *a = std::clamp(c[3], 0.0f, 1.0f);
}

// 以 900 px 高为基准的界面尺度（导出分辨率越高，线/字同比放大）
inline float uiScale(int h) {
    return std::max(0.5f, static_cast<float>(h) / 900.0f);
}

inline void fillTriangle(std::vector<uint8_t>& rgba, int w, int h, const glm::vec2& p0,
                         const glm::vec2& p1, const glm::vec2& p2, int r, int g, int b,
                         float a) {
    const float minX = std::floor(std::min({p0.x, p1.x, p2.x}));
    const float maxX = std::ceil(std::max({p0.x, p1.x, p2.x}));
    const float minY = std::floor(std::min({p0.y, p1.y, p2.y}));
    const float maxY = std::ceil(std::max({p0.y, p1.y, p2.y}));
    const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::fabs(area) < 1e-4f) {
        return;
    }
    for (int y = static_cast<int>(minY); y <= static_cast<int>(maxY); ++y) {
        for (int x = static_cast<int>(minX); x <= static_cast<int>(maxX); ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) / area;
            const float w1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 >= -1e-3f && w1 >= -1e-3f && w2 >= -1e-3f) {
                esp_overlay::putPixel(rgba, w, h, x, y, r, g, b, a);
            }
        }
    }
}

// 箭头头部：尖端 p1，方向 (dx,dy) 指向尖端
inline void drawHead(std::vector<uint8_t>& rgba, int w, int h, const glm::vec2& tip,
                     float ux, float uy, float size, int r, int g, int b, float a) {
    const float half = size * 0.45f;
    const glm::vec2 left(tip.x - ux * size - uy * half, tip.y - uy * size + ux * half);
    const glm::vec2 right(tip.x - ux * size + uy * half, tip.y - uy * size - ux * half);
    fillTriangle(rgba, w, h, tip, left, right, r, g, b, a);
}

} // namespace detail

// 画一条箭头（普通 / 波浪）。像素坐标输入，便于视口与导出共用同一实现。
inline void drawArrowOnImage(std::vector<uint8_t>& rgba, int w, int h, const glm::vec2& p0,
                             const glm::vec2& p1, const Arrow& arrow, float scale) {
    int r = 0, g = 0, b = 0;
    float a = 1.0f;
    detail::rgbaToBytes(arrow.color, &r, &g, &b, &a);
    const float len = glm::length(p1 - p0);
    if (len < 1e-3f) {
        return;
    }
    const float width = std::max(1.0f, arrow.width * scale);
    const float head = std::max(6.0f, 12.0f * arrow.headScale * scale);
    const glm::vec2 u = (p1 - p0) / len;

    if (arrow.style == ArrowStyle::Zigzag) {
        // 波浪：沿弦方向的正弦折线，振幅随线宽放大（"光子"箭头）。
        // 波长给得比早期版本长（22 px @900 基准）并加密采样：步长太短时
        // 整数取整会把正弦画成"方波楼梯"，示意图上很扎眼。
        const float amp = width * 1.8f;
        const float wave = std::max(14.0f, 22.0f * scale);
        const int steps = std::max(24, static_cast<int>(len / 1.2f));
        glm::vec2 prev = p0;
        const glm::vec2 n(-u.y, u.x);
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float phase = (t * len / wave) * 6.2831853f;
            const float off = (i == steps) ? 0.0f : std::sin(phase) * amp;
            const glm::vec2 cur = p0 + (p1 - p0) * t + n * off;
            esp_overlay::drawLine(rgba, w, h, static_cast<int>(prev.x),
                                  static_cast<int>(prev.y), static_cast<int>(cur.x),
                                  static_cast<int>(cur.y), r, g, b, a,
                                  static_cast<int>(std::lround(width)));
            prev = cur;
        }
        detail::drawHead(rgba, w, h, p1, u.x, u.y, head, r, g, b, a);
        if (arrow.doubleHead) {
            detail::drawHead(rgba, w, h, p0, -u.x, -u.y, head, r, g, b, a);
        }
        return;
    }

    // 普通箭头：线段缩进一个头长，避免头部覆盖线端造成钝头
    const glm::vec2 shaftEnd = p1 - u * (head * 0.85f);
    esp_overlay::drawLine(rgba, w, h, static_cast<int>(p0.x), static_cast<int>(p0.y),
                          static_cast<int>(shaftEnd.x), static_cast<int>(shaftEnd.y), r, g,
                          b, a, static_cast<int>(std::lround(width)));
    detail::drawHead(rgba, w, h, p1, u.x, u.y, head, r, g, b, a);
    if (arrow.doubleHead) {
        esp_overlay::drawLine(rgba, w, h, static_cast<int>(p0.x), static_cast<int>(p0.y),
                              static_cast<int>((p0 + u * (head * 0.85f)).x),
                              static_cast<int>((p0 + u * (head * 0.85f)).y), r, g, b, a,
                              static_cast<int>(std::lround(width)));
        detail::drawHead(rgba, w, h, p0, -u.x, -u.y, head, r, g, b, a);
    }
}

// 12 条棱：origin + 三个棱向量的组合
inline void drawBoxOnImage(std::vector<uint8_t>& rgba, int w, int h,
                           const glm::mat4& viewProj, const Box& box, float scale) {
    if (!box.visible) {
        return;
    }
    int r = 0, g = 0, b = 0;
    float a = 1.0f;
    detail::rgbaToBytes(box.color, &r, &g, &b, &a);
    glm::vec3 corner[8];
    for (int i = 0; i < 8; ++i) {
        corner[i] = box.origin + ((i & 1) ? box.a1 : glm::vec3(0.0f)) +
                    ((i & 2) ? box.a2 : glm::vec3(0.0f)) +
                    ((i & 4) ? box.a3 : glm::vec3(0.0f));
    }
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    const int thickness = std::max(1, static_cast<int>(std::lround(box.lineWidth * scale)));
    for (const auto& e : kEdges) {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!esp_overlay::projectToPixel(viewProj, corner[e[0]], w, h, x0, y0) ||
            !esp_overlay::projectToPixel(viewProj, corner[e[1]], w, h, x1, y1)) {
            continue;
        }
        esp_overlay::drawLine(rgba, w, h, static_cast<int>(std::lround(x0)),
                              static_cast<int>(std::lround(y0)),
                              static_cast<int>(std::lround(x1)),
                              static_cast<int>(std::lround(y1)), r, g, b, a, thickness);
    }
}

// 标签：锚点小圆点 + 字符（白字黑边 / 黑字白边随背景）+ 可选引导点
inline void drawLabelsOnImage(std::vector<uint8_t>& rgba, int w, int h,
                              const glm::mat4& viewProj,
                              const std::vector<Label>& labels, float scale,
                              bool darkBackground) {
    const int outline = darkBackground ? 0 : 255;
    for (const auto& label : labels) {
        if (!label.visible || label.text.empty()) {
            continue;
        }
        float fx = 0.0f, fy = 0.0f;
        if (!esp_overlay::projectToPixel(viewProj, label.pos, w, h, fx, fy)) {
            continue;
        }
        int r = 0, g = 0, b = 0;
        float a = 1.0f;
        detail::rgbaToBytes(label.color, &r, &g, &b, &a);
        const int px = std::max(8, static_cast<int>(std::lround(label.pixelSize * scale)));
        const int dot = std::max(2, px / 6);
        if (label.leader) {
            esp_overlay::drawDisc(rgba, w, h, static_cast<int>(std::lround(fx)),
                                  static_cast<int>(std::lround(fy)), dot + 1, outline,
                                  outline, outline, 0.85f);
            esp_overlay::drawDisc(rgba, w, h, static_cast<int>(std::lround(fx)),
                                  static_cast<int>(std::lround(fy)), dot, r, g, b, a);
        }
        const float tw = esp_overlay::detail::measureText(label.text.c_str(), px);
        float tx = fx + dot + 6.0f * scale;
        if (tx + tw > static_cast<float>(w - 4)) {
            tx = fx - dot - 6.0f * scale - tw;
        }
        const float baselineY = fy + px * 0.35f;
        esp_overlay::detail::drawText(
            rgba, w, h, tx, baselineY, label.text.c_str(), px,
            static_cast<uint8_t>(std::clamp(r, 0, 255)),
            static_cast<uint8_t>(std::clamp(g, 0, 255)),
            static_cast<uint8_t>(std::clamp(b, 0, 255)),
            static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f),
            std::max(1, px / 14), static_cast<uint8_t>(outline),
            static_cast<uint8_t>(outline), static_cast<uint8_t>(outline));
    }
}

// 水印：大号半透明文字；位置按锚点
inline void drawWatermarkOnImage(std::vector<uint8_t>& rgba, int w, int h,
                                 const Watermark& wm) {
    if (!wm.visible || wm.text.empty()) {
        return;
    }
    int r = 0, g = 0, b = 0;
    float a = 1.0f;
    detail::rgbaToBytes(wm.color, &r, &g, &b, &a);
    const int px = std::max(10, static_cast<int>(std::lround(wm.scale * static_cast<float>(h))));
    const float tw = esp_overlay::detail::measureText(wm.text.c_str(), px);
    const float margin = 0.03f * static_cast<float>(w);
    float x = 0.0f;
    float baselineY = 0.0f;
    switch (wm.anchor) {
        case Anchor::BottomLeft:
            x = margin;
            baselineY = static_cast<float>(h) - margin;
            break;
        case Anchor::Center:
            x = (static_cast<float>(w) - tw) * 0.5f;
            baselineY = static_cast<float>(h) * 0.5f + px * 0.35f;
            break;
        case Anchor::TopRight:
            x = static_cast<float>(w) - margin - tw;
            baselineY = static_cast<float>(margin) + px;
            break;
        case Anchor::BottomRight:
        default:
            x = static_cast<float>(w) - margin - tw;
            baselineY = static_cast<float>(h) - margin;
            break;
    }
    esp_overlay::detail::drawText(
        rgba, w, h, x, baselineY, wm.text.c_str(), px,
        static_cast<uint8_t>(std::clamp(r, 0, 255)),
        static_cast<uint8_t>(std::clamp(g, 0, 255)),
        static_cast<uint8_t>(std::clamp(b, 0, 255)),
        static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f),
        std::max(0, px / 30), 0, 0, 0);
}

// 主入口：把所有标注叠加到 RGBA8 缓冲（PNG 导出在 ESP 叠加之后调用）
inline void drawSceneOverlay(std::vector<uint8_t>& rgba, int w, int h,
                             const glm::mat4& viewProj, const Snapshot& snapshot,
                             bool darkBackground) {
    if (snapshot.empty() || w <= 0 || h <= 0 ||
        rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4u) {
        return;
    }
    const float scale = detail::uiScale(h);
    drawBoxOnImage(rgba, w, h, viewProj, snapshot.box, scale);
    for (const auto& arrow : snapshot.arrows) {
        if (!arrow.visible) {
            continue;
        }
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!esp_overlay::projectToPixel(viewProj, arrow.from, w, h, x0, y0) ||
            !esp_overlay::projectToPixel(viewProj, arrow.to, w, h, x1, y1)) {
            continue;
        }
        drawArrowOnImage(rgba, w, h, glm::vec2(x0, y0), glm::vec2(x1, y1), arrow, scale);
    }
    drawLabelsOnImage(rgba, w, h, viewProj, snapshot.labels, scale, darkBackground);
    drawWatermarkOnImage(rgba, w, h, snapshot.watermark);
}

} // namespace moleculens::engine::scene_overlay
