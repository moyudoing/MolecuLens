// =============================================================================
// MolecuLens · tests · engine/render/scene_bounds_tests.cpp —— 单原子相机取景
// 校验：单原子分子包围球半径 ≥ 原子 vdW 半径（相机不至于贴脸退化）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "engine/render/renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;

namespace {

entity::SceneStructure makeSingleAtomScene() {
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
    ts.positions.emplace_back(1.0f, 2.0f, 3.0f);
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, entity::Transform{}, std::move(traj));
    return scene;
}

} // namespace

TEST_CASE("Single-atom scene bounds radius is floored to vdW radius",
          "[engine][render]") {
    entity::SceneStructure scene = makeSingleAtomScene();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.refreshFrameData();

    glm::vec3 center;
    float radius = 0.0f;
    REQUIRE(renderer.sceneBounds(&center, &radius));
    // 包围球半径应 ≥ 氧的 vdW 半径（~1.5），否则相机距离退化
    REQUIRE(radius >= 1.4f);
    REQUIRE(center == glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("Empty scene has no bounds", "[engine][render]") {
    entity::SceneStructure scene;
    engine::Renderer renderer;
    renderer.setScene(&scene);
    glm::vec3 c;
    float r = 0.0f;
    REQUIRE_FALSE(renderer.sceneBounds(&c, &r));
}
