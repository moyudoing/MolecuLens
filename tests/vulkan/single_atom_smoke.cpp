// =============================================================================
// MolecuLens · tests · vulkan/single_atom_smoke.cpp —— 单原子渲染冒烟
// 校验：单原子（包围球半径 0）经 sceneBounds 修正后，VDW/表面等模式
// 用合理相机距离渲染出像素（此前相机贴在原子上 → 不可见）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

#include "spdlog/spdlog.h"

using namespace moleculens;

namespace {

// 非背景像素计数：背景参考色取左上角像素（几何恒居中，四角必为背景）。
// 不用绝对阈值——离屏颜色附件自 P6 起是 _SRGB（硬件写时做 linear→sRGB 编码），
// 背景字节值随之上移，固定阈值（>30）会把**整个画面**都算成非背景，
// 冒烟测试也就失去判别力（曾实测 nonBg == 宽×高）。
uint32_t countNonBackground(const std::vector<uint8_t>& px, int tol = 24) {
    if (px.size() < 4) {
        return 0;
    }
    const uint8_t* bg = px.data();   // 左上角 = 背景
    uint32_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (std::abs(static_cast<int>(px[i]) - bg[0]) > tol ||
            std::abs(static_cast<int>(px[i + 1]) - bg[1]) > tol ||
            std::abs(static_cast<int>(px[i + 2]) - bg[2]) > tol) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("Single atom renders in spacefill with corrected camera",
          "[vulkan][smoke][single-atom]") {
    // 场景：单个氧原子
    entity::SceneStructure scene;
    entity::MoleculeStructure ms;
    ms.elements.push_back(entity::ElementType::Oxygen);
    entity::Atom a;
    a.element = entity::ElementType::Oxygen;
    ms.atoms.push_back(a);
    ms.atomNameIds.push_back(0);
    ms.key = entity::computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<entity::Trajectory>();
    entity::Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, entity::Transform{}, std::move(traj));

    // 用 sceneBounds（含半径下限修复）自动取景
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.refreshFrameData();
    glm::vec3 center;
    float radius = 0.0f;
    REQUIRE(renderer.sceneBounds(&center, &radius));
    engine::OrbitCamera cam;
    cam.centerOn(center, radius);

    // 单原子 VDW（空间填充）球
    engine::SceneCommandList list;
    engine::MaterialBlock mat;
    mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    mat.ambient = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    list.append(engine::Cmd::MaterialBlock, &mat, sizeof(mat));
    engine::SphereInst inst;
    inst.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 1.52f);   // O vdW 半径
    inst.color = glm::vec4(1.0f, 0.2f, 0.2f, 1.0f);
    inst.pickId = glm::uvec2(0, 0);
    inst.pad = glm::uvec2(0, 0);
    list.append(engine::Cmd::SphereInst, &inst, sizeof(inst));

    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    engine::OffscreenRenderer off;
    std::string err;
    if (!off.init(cfg, 128, 96, &err)) {
        FAIL("offscreen init failed: " << err);
    }
    engine::RenderView view;
    view.camera = cam;
    view.width = 128;
    view.height = 96;
    off.execute(list, view);

    std::vector<uint8_t> pixels;
    REQUIRE(off.readbackPixels(pixels, engine::PixelFormat::Rgba8));
    const uint32_t nonBg = countNonBackground(pixels);
    spdlog::info("[test] single-atom spacefill nonBg={} radius={}", nonBg, radius);
    // 单原子应占视口可观面积（半径已修正，非退化相机）
    REQUIRE(nonBg > 100);
}

// 同一命令流连续执行两次：第二次 serial 未变 → 走"跳过重传"路径
// （draw_geometry 的 uploadRevision 命中）。两条路径必须逐像素一致——
// 若跳过时缓冲内容不是上次那份，这里会立刻显形。
TEST_CASE("Repeat execute of an unchanged command list is pixel-identical",
          "[vulkan][smoke][single-atom]") {
    engine::SceneCommandList list;
    engine::MaterialBlock mat;
    mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    mat.opacity = 1.0f;
    list.append(engine::Cmd::MaterialBlock, &mat, sizeof(mat));
    engine::SphereInst inst;
    inst.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    inst.color = glm::vec4(0.9f, 0.5f, 0.2f, 1.0f);
    list.append(engine::Cmd::SphereInst, &inst, sizeof(inst));

    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    engine::OffscreenRenderer off;
    std::string err;
    if (!off.init(cfg, 128, 96, &err)) {
        FAIL("offscreen init failed: " << err);
    }
    engine::RenderView view;
    view.camera.centerOn(glm::vec3(0.0f), 1.0f);
    view.width = 128;
    view.height = 96;

    off.execute(list, view);            // 首次：全量上传
    std::vector<uint8_t> first;
    REQUIRE(off.readbackPixels(first, engine::PixelFormat::Rgba8));

    off.execute(list, view);            // 再次：serial 未变 → 跳过上传
    std::vector<uint8_t> second;
    REQUIRE(off.readbackPixels(second, engine::PixelFormat::Rgba8));

    REQUIRE(first.size() == second.size());
    REQUIRE(first == second);
}
