// =============================================================================
// MolecuLens · tests · engine/export/esp_overlay_tests.cpp
// ESP 色标条/极值标签栅格化的纯逻辑测试（engine/export/overlay/esp_overlay_raster.h）。
//
// 覆盖：overlay inactive 不改像素；active 时色标条区域与极值标签区域出现变化；
//       缓冲尺寸不足时安全返回；invert/色标不同的色标条像素不同。
// 说明：该栅格化实现被 CLI 出图与 GUI 导出 PNG 共用，属于"导出图带标注"的唯一
//       实现；纯 CPU 逻辑，适合单元测试。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/export/overlay/annotation/scene_overlay_raster.h"
#include "engine/export/overlay/esp_overlay_raster.h"
#include "engine/style/espsurface/esp_surface.h"

using namespace moleculens;
using namespace moleculens::engine;

namespace {

// 400x300 纯白 RGBA 缓冲
std::vector<uint8_t> whiteImage(int w, int h) {
    return std::vector<uint8_t>(static_cast<std::size_t>(w) * h * 4, 255);
}

// 单位矩阵：世界 (0,0,0) → NDC (0,0,0) → 图像中心（够用于标签/坐标断言）
const glm::mat4 kIdentity(1.0f);

std::size_t countDifferent(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::size_t n = 0;
    const std::size_t pixels = a.size() / 4;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t o = i * 4;
        if (a[o] != b[o] || a[o + 1] != b[o + 1] || a[o + 2] != b[o + 2]) {
            ++n;
        }
    }
    return n;
}

// 某个矩形区域内是否存在与参考缓冲不同的像素
bool regionChanged(const std::vector<uint8_t>& img, const std::vector<uint8_t>& ref,
                   int w, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 4;
            if (img[o] != ref[o] || img[o + 1] != ref[o + 1] || img[o + 2] != ref[o + 2]) {
                return true;
            }
        }
    }
    return false;
}

EspOverlay makeOverlay(EspColormap colormap = EspColormap::Rwb, bool invert = false) {
    EspOverlay ov;
    ov.active = true;
    ov.colormap = colormap;
    ov.vminKcal = -40.0f;
    ov.vmaxKcal = 40.0f;
    ov.invert = invert;
    ov.extrema.push_back(EspExtremum{glm::vec3(0.0f), 21.5f, true});
    ov.extrema.push_back(EspExtremum{glm::vec3(0.0f), -33.0f, false});
    return ov;
}

} // namespace

TEST_CASE("ESP overlay inactive leaves the image untouched", "[engine][espsurface][export]") {
    constexpr int w = 400, h = 300;
    auto img = whiteImage(w, h);
    const auto ref = img;
    const EspOverlay ov;   // active=false
    esp_overlay::drawEspOverlay(img, w, h, kIdentity, ov, /*darkBackground=*/false);
    REQUIRE(countDifferent(img, ref) == 0);
}

TEST_CASE("ESP overlay draws colorbar and extremum labels", "[engine][espsurface][export]") {
    constexpr int w = 400, h = 300;
    auto img = whiteImage(w, h);
    const auto ref = img;
    const EspOverlay ov = makeOverlay();
    esp_overlay::drawEspOverlay(img, w, h, kIdentity, ov, /*darkBackground=*/false);

    // 整体确有像素变化
    REQUIRE(countDifferent(img, ref) > 100);
    // 右侧色标条区域（barX≈w-w/10，宽约 w/90 → 取一个保守窗口）
    REQUIRE(regionChanged(img, ref, w, w * 3 / 4, h / 4, w, h * 3 / 4));
    // 图像中心：极值点标签（世界原点投影到中心）
    REQUIRE(regionChanged(img, ref, w, w / 2 - 60, h / 2 - 20, w / 2 + 60, h / 2 + 20));
}

TEST_CASE("ESP overlay invert / colormap changes colorbar pixels",
          "[engine][espsurface][export]") {
    constexpr int w = 400, h = 300;
    auto normal = whiteImage(w, h);
    auto inverted = whiteImage(w, h);
    auto viridis = whiteImage(w, h);
    esp_overlay::drawEspOverlay(normal, w, h, kIdentity, makeOverlay(EspColormap::Rwb, false),
                                false);
    esp_overlay::drawEspOverlay(inverted, w, h, kIdentity,
                                makeOverlay(EspColormap::Rwb, true), false);
    esp_overlay::drawEspOverlay(viridis, w, h, kIdentity,
                                makeOverlay(EspColormap::Viridis, false), false);
    REQUIRE(countDifferent(normal, inverted) > 0);
    REQUIRE(countDifferent(normal, viridis) > 0);
}

TEST_CASE("ESP overlay tolerates undersized or empty buffers",
          "[engine][espsurface][export]") {
    const EspOverlay ov = makeOverlay();
    std::vector<uint8_t> tiny(16, 255);
    esp_overlay::drawEspOverlay(tiny, 400, 300, kIdentity, ov, false);   // 不崩溃
    REQUIRE(tiny.size() == 16);
    std::vector<uint8_t> empty;
    esp_overlay::drawEspOverlay(empty, 400, 300, kIdentity, ov, false);
    REQUIRE(empty.empty());
}

// 追加：场景标注栅格化（水盒/标签/箭头/水印）的纯逻辑测试。
// 与 ESP 叠加测试同源：同一个 RGBA 缓冲 + 单位视图投影矩阵，验证"该画的画了、
// 不该画的不改像素、缓冲区过小不崩"。

TEST_CASE("scene overlay draws box edges and respects visibility",
          "[engine][export][annotation]") {
    const int w = 400;
    const int h = 300;
    const auto blank = whiteImage(w, h);
    auto img = blank;
    scene_overlay::Snapshot snap;
    snap.box.visible = true;
    snap.box.origin = glm::vec3(-1.0f, -1.0f, -1.0f);
    snap.box.a1 = glm::vec3(2.0f, 0.0f, 0.0f);
    snap.box.a2 = glm::vec3(0.0f, 2.0f, 0.0f);
    snap.box.a3 = glm::vec3(0.0f, 0.0f, 2.0f);
    scene_overlay::drawSceneOverlay(img, w, h, kIdentity, snap, false);
    REQUIRE(countDifferent(img, blank) > 0);

    // 不可见盒：不改任何像素
    auto img2 = blank;
    scene_overlay::Snapshot hidden;
    scene_overlay::drawSceneOverlay(img2, w, h, kIdentity, hidden, false);
    REQUIRE(countDifferent(img2, blank) == 0);
}

TEST_CASE("scene overlay arrow styles differ and double head doubles ink",
          "[engine][export][annotation]") {
    const int w = 400;
    const int h = 300;
    auto plain = whiteImage(w, h);
    auto zigzag = whiteImage(w, h);
    auto doubleHead = whiteImage(w, h);
    scene_overlay::Snapshot snap;
    scene_overlay::Arrow a;
    a.from = glm::vec3(-0.6f, 0.0f, 0.0f);
    a.to = glm::vec3(0.6f, 0.0f, 0.0f);
    a.width = 2.0f;
    snap.arrows.push_back(a);
    scene_overlay::drawSceneOverlay(plain, w, h, kIdentity, snap, false);

    snap.arrows[0].style = scene_overlay::ArrowStyle::Zigzag;
    scene_overlay::drawSceneOverlay(zigzag, w, h, kIdentity, snap, false);

    snap.arrows[0].style = scene_overlay::ArrowStyle::Plain;
    snap.arrows[0].doubleHead = true;
    scene_overlay::drawSceneOverlay(doubleHead, w, h, kIdentity, snap, false);

    REQUIRE(countDifferent(plain, zigzag) > 0);
    // 双头箭头左侧起点附近应比单头多出像素（起点 x=-0.6 → 像素约 0.2·w）
    REQUIRE(regionChanged(doubleHead, plain, w, w / 2 - 125, h / 2 - 35, w / 2 - 45,
                          h / 2 + 35));
}

TEST_CASE("scene overlay label and watermark rasterize", "[engine][export][annotation]") {
    const int w = 480;
    const int h = 320;
    const auto blank = whiteImage(w, h);

    auto labelImg = blank;
    scene_overlay::Snapshot withLabel;
    scene_overlay::Label label;
    label.text = "LH-I";
    label.pos = glm::vec3(0.0f, 0.0f, 0.0f);
    label.pixelSize = 24.0f;
    withLabel.labels.push_back(label);
    scene_overlay::drawSceneOverlay(labelImg, w, h, kIdentity, withLabel, false);
    REQUIRE(countDifferent(labelImg, blank) > 0);

    auto wmImg = blank;
    scene_overlay::Snapshot withWm;
    withWm.watermark.visible = true;
    withWm.watermark.text = "TCBG";
    withWm.watermark.scale = 0.2f;
    withWm.watermark.color[3] = 0.35f;
    scene_overlay::drawSceneOverlay(wmImg, w, h, kIdentity, withWm, false);
    REQUIRE(regionChanged(wmImg, blank, w, w - 220, h - 100, w - 1, h - 1));
}

TEST_CASE("scene overlay tolerates undersized buffers", "[engine][export][annotation]") {
    std::vector<uint8_t> tiny(16, 255);
    scene_overlay::Snapshot snap;
    snap.box.visible = true;
    snap.watermark.visible = true;
    snap.watermark.text = "x";
    scene_overlay::drawSceneOverlay(tiny, 400, 300, kIdentity, snap, false);
    REQUIRE(tiny.size() == 16);
}
