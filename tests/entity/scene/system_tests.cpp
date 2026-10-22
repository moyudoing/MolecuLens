// =============================================================================
// MolecuLens · tests · entity/scene/system_tests.cpp —— 体系（组）数据结构
// 规格来源：一.1 体系 = 一次导入的多分子文件；一.3 跨组移交/重组；
//           一.4 体系居中；一.5 位姿变更重算世界坐标
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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

// 建一个"两原子分子"的实例：两原子化学上相邻（键长 1 Å），但按给定坐标摆放，
// 用于构造"跨盒子边界被拆开"的情形。box 为 0 表示非周期。
uint32_t addDiatomic(SceneStructure& scene, const glm::vec3& a, const glm::vec3& b,
                     const std::array<float, 9>& box, uint32_t systemId) {
    MoleculeStructure ms;
    for (int i = 0; i < 2; ++i) {
        ms.elements.push_back(ElementType::Oxygen);
        Atom at;
        at.element = ElementType::Oxygen;
        at.chainId = 0;
        at.residueId = 0;
        ms.atoms.push_back(at);
        ms.atomNameIds.push_back(0);
    }
    ms.bonds.push_back(Bond{0, 1});
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions = {a, b};
    ts.box = box;
    traj->frames.push_back(std::move(ts));
    return scene.addTrajectoryInstance(si, Transform{}, std::move(traj), systemId);
}

} // namespace

// =============================================================================
// PBC 分子补齐（unwrap）：跨盒子边界的分子必须被拉成连续几何。
// 不补齐的后果不是"坐标略有偏差"，而是渲染出一根接近盒长的长棍 —— 判键用最小
// 镜像是对的，渲染却按原始坐标连键。实测 data/md.gro 有 20 根（盒长 40.28 Å）。
// =============================================================================
TEST_CASE("Periodic box: molecule split across the boundary is unwrapped", "[entity][scene][pbc]") {
    std::array<float, 9> box{};
    box[0] = box[4] = box[8] = 40.0f;

    SceneStructure scene;
    const uint32_t sysId = scene.addSystem("P");
    // 两原子相距 1 Å，但分处盒子两端（z = 0.5 与 z = 39.5）
    const uint32_t inst = addDiatomic(scene, glm::vec3(0, 0, 0.5f),
                                      glm::vec3(0, 0, 39.5f), box, sysId);
    REQUIRE(inst == 0);
    REQUIRE(scene.applyTrajectoryFrame(inst, 0));

    const auto& p = scene.coordsSoA().positions;
    REQUIRE(p.size() == 2);
    // 补齐后应贴在一起（1 Å），而不是跨越整个盒子（39 Å）
    REQUIRE(glm::length(p[1] - p[0]) == Catch::Approx(1.0f).margin(1e-3f));
}

TEST_CASE("Periodic box: each molecule is unwrapped around its own anchor",
          "[entity][scene][pbc]") {
    // 回归：全体系共用一个锚原子时，靠近半个盒长处的分子会被拆成一根长棍
    // （锚在 0.5，B 分子 19.0/20.2 各自选到相反的镜像）。必须按分子连通
    // 分量分别锚定，B 分子也要保持 1.2 Å 键长。
    std::array<float, 9> box{};
    box[0] = box[4] = box[8] = 40.0f;
    SceneStructure scene;
    const uint32_t sysId = scene.addSystem("P2");
    addDiatomic(scene, glm::vec3(0.5f, 0, 0), glm::vec3(1.5f, 0, 0), box, sysId);
    const uint32_t instB = addDiatomic(scene, glm::vec3(19.0f, 0, 0),
                                      glm::vec3(20.2f, 0, 0), box, sysId);
    REQUIRE(scene.applyTrajectoryFrame(instB, 0));

    const auto& p = scene.coordsSoA().positions;
    REQUIRE(p.size() == 4);
    REQUIRE(glm::length(p[1] - p[0]) == Catch::Approx(1.0f).margin(1e-3f));
    REQUIRE(glm::length(p[3] - p[2]) == Catch::Approx(1.2f).margin(1e-3f));
}

TEST_CASE("Periodic box: no box means coordinates are left untouched", "[entity][scene][pbc]") {
    // 回归护栏：非周期数据（box 全 0）绝不能被位移 —— 常见情形，静默改动最危险
    std::array<float, 9> noBox{};
    SceneStructure scene;
    const uint32_t sysId = scene.addSystem("N");
    const glm::vec3 a(0, 0, 0.5f), b(0, 0, 39.5f);
    const uint32_t inst = addDiatomic(scene, a, b, noBox, sysId);
    REQUIRE(scene.applyTrajectoryFrame(inst, 0));

    const auto& p = scene.coordsSoA().positions;
    REQUIRE(p[0] == a);
    REQUIRE(p[1] == b);
}

TEST_CASE("Minimum-image delta wraps to the shortest displacement", "[entity][scene][pbc]") {
    const float L[3] = {10.0f, 10.0f, 10.0f};
    // 9 Å 的原始差 → 最小镜像取 -1 Å（跨越边界更近）
    const glm::vec3 d = minimumImageDelta(glm::vec3(0, 0, 9.0f), L);
    REQUIRE(d.z == Catch::Approx(-1.0f));
    // 4 Å 的原始差 → 保持原样（盒子内就是最短）
    REQUIRE(minimumImageDelta(glm::vec3(0, 0, 4.0f), L).z == Catch::Approx(4.0f));
    // 无盒子（全 0）→ 恒等
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    REQUIRE(minimumImageDelta(glm::vec3(1, 2, 3), zero) == glm::vec3(1, 2, 3));
}

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

