// =============================================================================
// MolecuLens · tests · vulkan/line_smoke.cpp —— 线段渲染冒烟（线框模式）
// 校验：单条线段是否真的写入像素（定位线框模式不显示的问题）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"

#include "spdlog/spdlog.h"

using namespace moleculens::engine;

TEST_CASE("OffscreenRenderer renders a line segment", "[vulkan][smoke][line]") {
    RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    OffscreenRenderer renderer;
    std::string err;
    if (!renderer.init(cfg, 128, 96, &err)) {
        WARN("Skipping line smoke (no Vulkan device): " << err);
        return;
    }

    SceneCommandList list;
    MaterialBlock mat;
    mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));
    LineInst line;
    line.aPos = glm::vec4(-5.0f, -5.0f, 0.0f, 0.0f);
    line.bPos = glm::vec4(5.0f, 5.0f, 0.0f, 0.0f);
    line.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    list.append(Cmd::LineInst, &line, sizeof(line));

    RenderView view;
    view.camera.centerOn({0, 0, 0}, 10.0f);
    view.width = 128;
    view.height = 96;
    spdlog::info("[test] before execute");
    renderer.execute(list, view);
    spdlog::info("[test] after execute");
    std::vector<uint8_t> pixels;
    const bool rb = renderer.readbackPixels(pixels, PixelFormat::Rgba8);
    spdlog::info("[test] after readback rb={}", rb);
    REQUIRE(rb);
    // 非背景计数（背景参考 = 左上角像素）。不用绝对阈值：离屏颜色附件自 P6 起
    // 为 _SRGB（写时编码），背景字节值上移，固定阈值会失去判别力。
    const uint8_t* bgp = pixels.data();
    uint32_t nonBg = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (std::abs(static_cast<int>(pixels[i]) - bgp[0]) > 24 ||
            std::abs(static_cast<int>(pixels[i + 1]) - bgp[1]) > 24 ||
            std::abs(static_cast<int>(pixels[i + 2]) - bgp[2]) > 24) {
            ++nonBg;
        }
    }
    // 线跨整个视口对角线 → 应有很多亮像素；0 说明线渲染失败
    spdlog::info("[test] line smoke nonBg={}", nonBg);
    REQUIRE(nonBg > 10);
}
