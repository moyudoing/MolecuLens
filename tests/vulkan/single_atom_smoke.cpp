// =============================================================================
// MolecuLens · tests · vulkan/single_atom_smoke.cpp —— 单原子渲染冒烟
// 校验：单原子（包围球半径 0）经 sceneBounds 修正后，VDW/表面等模式
// 用合理相机距离渲染出像素（此前相机贴在原子上 → 不可见）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

#include "spdlog/spdlog.h"

using namespace moleculens;

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
    uint32_t nonBg = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] > 30 || pixels[i + 1] > 30 || pixels[i + 2] > 30) {
            ++nonBg;
        }
    }
    spdlog::info("[test] single-atom spacefill nonBg={} radius={}", nonBg, radius);
    // 单原子应占视口可观面积（半径已修正，非退化相机）
    REQUIRE(nonBg > 100);
}
