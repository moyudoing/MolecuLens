// =============================================================================
// MolecuLens · tests · entity/scene/system_tests.cpp —— 体系（组）数据结构
// 规格来源：一.1 体系 = 一次导入的多分子文件；一.3 跨组移交/重组；
//           一.4 体系居中；一.5 位姿变更重算世界坐标
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::entity;

namespace {

// 建一个单原子（O，坐标可指定）的实例
uint32_t addAtom(SceneStructure& scene, const glm::vec3& pos, uint32_t systemId) {
    MoleculeStructure ms;
    ms.elements.push_back(ElementType::Oxygen);
    Atom a;
    a.element = ElementType::Oxygen;
    ms.atoms.push_back(a);
    ms.atomNameIds.push_back(0);
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.push_back(pos);
    traj->frames.push_back(std::move(ts));
    return scene.addTrajectoryInstance(si, Transform{}, std::move(traj), systemId);
}

} // namespace

TEST_CASE("Add system and instances belong to it", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    const uint32_t sysB = scene.addSystem("B");
    REQUIRE(scene.systemIndexById(sysA) != UINT32_MAX);
    REQUIRE(scene.systemIndexById(sysB) != UINT32_MAX);

    const uint32_t i0 = addAtom(scene, glm::vec3(0.0f), sysA);
    const uint32_t i1 = addAtom(scene, glm::vec3(1.0f), sysA);
    const uint32_t i2 = addAtom(scene, glm::vec3(2.0f), sysB);

    REQUIRE(scene.systemIdOfInstance(i0) == sysA);
    REQUIRE(scene.systemIdOfInstance(i1) == sysA);
    REQUIRE(scene.systemIdOfInstance(i2) == sysB);
    REQUIRE(scene.systems()[0].instanceIndices.size() == 2u);
    REQUIRE(scene.systems()[1].instanceIndices.size() == 1u);
}

TEST_CASE("Move instance across systems (regroup / transfer)", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    const uint32_t sysB = scene.addSystem("B");
    const uint32_t i0 = addAtom(scene, glm::vec3(0.0f), sysA);

    REQUIRE(scene.moveInstanceToSystem(i0, sysB));
    REQUIRE(scene.systemIdOfInstance(i0) == sysB);
    REQUIRE(scene.systems()[0].instanceIndices.empty());
    REQUIRE(scene.systems()[1].instanceIndices.size() == 1u);

    // 无效目标体系 → 拒绝
    REQUIRE_FALSE(scene.moveInstanceToSystem(i0, 999u));
    REQUIRE(scene.systemIdOfInstance(i0) == sysB);
}

TEST_CASE("Remove system removes its instances and fixes indices", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    const uint32_t sysB = scene.addSystem("B");
    addAtom(scene, glm::vec3(0.0f), sysA);
    addAtom(scene, glm::vec3(1.0f), sysA);
    addAtom(scene, glm::vec3(2.0f), sysB);

    REQUIRE(scene.removeSystem(sysA));
    // 体系 A 与其实例全部删除
    REQUIRE(scene.systemIndexById(sysA) == UINT32_MAX);
    REQUIRE(scene.instances().size() == 1u);
    REQUIRE(scene.instances()[0].systemId == sysB);
    REQUIRE(scene.systems().size() == 1u);
    // 保留实例下标已前移（原来在下标 kept=2，现在 0）
    REQUIRE(scene.systemIdOfInstance(0) == sysB);
}

TEST_CASE("System transform composes into world coordinates", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    // 原子局部坐标 (1,2,3)；体系平移 (+10,0,0) → 世界坐标 (11,2,3)
    const uint32_t i0 = addAtom(scene, glm::vec3(1.0f, 2.0f, 3.0f), sysA);
    REQUIRE(scene.coordsSoA().positions[i0] == glm::vec3(1.0f, 2.0f, 3.0f));

    Transform sysTm;
    sysTm.translation = glm::vec3(10.0f, 0.0f, 0.0f);
    REQUIRE(scene.setSystemTransform(sysA, sysTm));
    REQUIRE(scene.coordsSoA().positions[i0] == glm::vec3(11.0f, 2.0f, 3.0f));
}

TEST_CASE("Instance transform change rebuilds world coordinates", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    const uint32_t i0 = addAtom(scene, glm::vec3(0.0f, 0.0f, 0.0f), sysA);
    REQUIRE(scene.coordsSoA().positions[i0] == glm::vec3(0.0f));

    Transform tm;
    tm.translation = glm::vec3(5.0f, 0.0f, 0.0f);
    scene.setInstanceTransform(i0, tm);   // 必须重算世界坐标（此前不重建 → 移动不生效）
    REQUIRE(scene.coordsSoA().positions[i0] == glm::vec3(5.0f, 0.0f, 0.0f));
}

TEST_CASE("Instance without system auto-creates a system", "[entity][system]") {
    SceneStructure scene;
    // 无体系、无当前体系 → addTrajectoryInstance(默认 UINT32_MAX) 自动新建
    const uint32_t i0 = addAtom(scene, glm::vec3(1.0f), UINT32_MAX);
    REQUIRE(scene.systems().size() == 1u);
    REQUIRE(scene.systemIdOfInstance(i0) != UINT32_MAX);
    REQUIRE(scene.systemIndexById(scene.systemIdOfInstance(i0)) == 0u);
}

TEST_CASE("System visibility toggles renderer skip", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    const uint32_t i0 = addAtom(scene, glm::vec3(0.0f), sysA);
    REQUIRE(scene.systems()[0].visible);
    REQUIRE(scene.setSystemVisible(sysA, false));
    REQUIRE_FALSE(scene.systems()[0].visible);
    // 实例仍存在，仅体系显隐变化
    REQUIRE(scene.instances().size() == 1u);
    REQUIRE(scene.instances()[i0].systemId == sysA);
}

TEST_CASE("Set system render mode (solid/wireframe/transparent/point)", "[entity][system]") {
    SceneStructure scene;
    const uint32_t sysA = scene.addSystem("A");
    addAtom(scene, glm::vec3(0.0f), sysA);
    REQUIRE(scene.systems()[0].renderMode == SystemRenderMode::Solid);
    REQUIRE(scene.setSystemRenderMode(sysA, SystemRenderMode::Wireframe));
    REQUIRE(scene.systems()[0].renderMode == SystemRenderMode::Wireframe);
    REQUIRE(scene.setSystemRenderMode(sysA, SystemRenderMode::Transparent));
    REQUIRE(scene.systems()[0].renderMode == SystemRenderMode::Transparent);
    REQUIRE(scene.setSystemRenderMode(sysA, SystemRenderMode::Point));
    REQUIRE(scene.systems()[0].renderMode == SystemRenderMode::Point);
    // 无效体系拒绝
    REQUIRE_FALSE(scene.setSystemRenderMode(999u, SystemRenderMode::Solid));
}
