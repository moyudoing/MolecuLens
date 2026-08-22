// =============================================================================
// MolecuLens · tests · vulkan/offscreen_smoke.cpp —— 离屏渲染冒烟
// 规格来源：主方案 §14.3（加载夹具 → 渲染一帧 → 校验非空图像 + 像素统计；
//           无窗口环境；无 GPU 时跳过）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"   // RenderView
#include "engine/vulkan/offscreen/offscreen_renderer.h"

using namespace moleculens::engine;

TEST_CASE("OffscreenRenderer renders and reads back non-empty pixels", "[vulkan][smoke]") {
    RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    OffscreenRenderer renderer;
    std::string err;
    if (!renderer.init(cfg, 128, 96, &err)) {
        WARN("Skipping offscreen smoke (no Vulkan device): " << err);
        return;
    }

    // 构造简单命令流：一个球
    SceneCommandList list;
    MaterialBlock mat;
    mat.ambient = glm::vec4(0.6f, 0.6f, 0.6f, 1);
    mat.diffuse = glm::vec4(0.9f, 0.9f, 0.9f, 1);
    mat.specular = glm::vec4(0.3f, 0.3f, 0.3f, 1);
    mat.shininess = 32.0f;
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));
    SphereInst inst;
    inst.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 10.0f);
    inst.color = glm::vec4(1.0f, 0.2f, 0.2f, 1.0f);
    inst.pickId = glm::uvec2(1, 0);
    inst.pad = glm::uvec2(0, 0);
    list.append(Cmd::SphereInst, &inst, sizeof(inst));

    RenderView view;
    view.camera.centerOn({0, 0, 0}, 10.0f);
    view.width = 128;
    view.height = 96;
    renderer.execute(list, view);

    std::vector<uint8_t> pixels;
    REQUIRE(renderer.readbackPixels(pixels, PixelFormat::Rgba8));
    REQUIRE(pixels.size() == 128u * 96u * 4u);

    // 像素统计：应有大量非背景像素（球投影）
    uint32_t nonBg = 0;
    uint32_t reddish = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const bool isBg = pixels[i] < 20 && pixels[i + 1] < 20 && pixels[i + 2] < 25;
        if (!isBg) {
            ++nonBg;
        }
        // 球色为红色调（R > B + 30；照明后红球仍保持色相）
        if (pixels[i] > pixels[i + 2] + 30 && pixels[i] > 50) {
            ++reddish;
        }
    }
    REQUIRE(nonBg > 100);      // 球占据视口大部分
    REQUIRE(reddish > 100);    // 存在大量红球像素（光照下 R 通道占优）
}
