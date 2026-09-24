// =============================================================================
// MolecuLens · tests · engine/rep/render_mode_tests.cpp —— 体系渲染模式命令
// 规格来源：新一.2 每体系独立渲染模式（实心/线框/透明/点）→ 后端切换 VkPipeline
// 校验：Representer 依据体系渲染模式在命令流中发射 Cmd::RenderMode。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::entity;

namespace {

// 建一个含单原子实例的体系，设置渲染模式后生成命令流
engine::SceneCommandList generateForSystem(SystemRenderMode mode) {
    SceneStructure scene;
    const uint32_t sysId = scene.addSystem("A");

    MoleculeStructure ms;
    ms.elements.push_back(ElementType::Carbon);
    Atom a;
    a.element = ElementType::Carbon;
    ms.atoms.push_back(a);
    ms.atomNameIds.push_back(0);
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    const uint32_t instIdx =
        scene.addTrajectoryInstance(si, Transform{}, std::move(traj), sysId);
    REQUIRE(scene.setSystemRenderMode(sysId, mode));

    engine::Representation rep;
    rep.name = "ballstick";
    rep.type = engine::RepresentationType::BallAndStick;

    entity::FrameData frame;
    frame.positions = &scene.coordsSoA().positions;
    std::vector<bool> mask(scene.coordsSoA().atomIds.size(), true);

    engine::SceneCommandList out;
    engine::Representer r;
    r.generate(rep, scene, scene.instances()[instIdx], instIdx, frame, 0, mask, out);
    return out;
}

} // namespace

TEST_CASE("Representer emits system render mode command", "[engine][rep]") {
    const auto list = generateForSystem(SystemRenderMode::Wireframe);
    bool found = false;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::RenderMode) {
            engine::RenderModeInst rmi{};
            std::memcpy(&rmi, it.payload(), sizeof(rmi));
            found = true;
            REQUIRE(rmi.mode == 1);   // Wireframe
        }
    }
    REQUIRE(found);
}

TEST_CASE("Representer emits point mode command", "[engine][rep]") {
    const auto list = generateForSystem(SystemRenderMode::Point);
    bool found = false;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::RenderMode) {
            engine::RenderModeInst rmi{};
            std::memcpy(&rmi, it.payload(), sizeof(rmi));
            found = true;
            REQUIRE(rmi.mode == 3);   // Point
        }
    }
    REQUIRE(found);
}

namespace {

// 统计某类型命令条数（SphereInst 原子球 / TriMesh 三角网格）
uint32_t countCmd(engine::SceneCommandList& list, engine::Cmd code) {
    uint32_t n = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == code) n++;
    }
    return n;
}

// 取首条 SphereInst 的半径（Vdw 全半径 / CPK 缩放半径验证）
bool firstSphereRadius(engine::SceneCommandList& list, float* outR) {
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::SphereInst && it.payloadSize() >= sizeof(engine::SphereInst)) {
            const auto* s = reinterpret_cast<const engine::SphereInst*>(it.payload());
            *outR = s->posRadius.w;
            return true;
        }
    }
    return false;
}

// 按表示类型生成单碳原子命令流
engine::SceneCommandList generateForType(engine::RepresentationType type) {
    SceneStructure scene;
    MoleculeStructure ms;
    ms.elements.push_back(ElementType::Carbon);
    Atom a;
    a.element = ElementType::Carbon;
    ms.atoms.push_back(a);
    ms.atomNameIds.push_back(0);
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    const uint32_t instIdx = scene.addTrajectoryInstance(si, Transform{}, std::move(traj));

    engine::Representation rep;
    rep.type = type;
    rep.radiusScale = 0.5f;   // CPK 缩放 0.5；VDW 应忽略该缩放

    entity::FrameData frame;
    frame.positions = &scene.coordsSoA().positions;
    std::vector<bool> mask(scene.coordsSoA().atomIds.size(), true);

    engine::SceneCommandList out;
    engine::Representer r;
    r.generate(rep, scene, scene.instances()[instIdx], instIdx, frame, 0, mask, out);
    return out;
}

} // namespace

// —— kDrawTable 对齐回归（历史 bug：表长 8 项但枚举 Count=9）——
// 修复前：VDW 槽位(index 7)误挂 drawQuickSurf（VDW 画出高斯表面而非范德华球）；
//         QuickSurf(index 8)落在表末越界 → 空函数指针 → 选择「快速表面」崩溃。
// 修复后：VDW → 范德华球（SphereInst），QuickSurf → TriMesh。
TEST_CASE("Vdw dispatches to vdW spheres (full radius, ignores radiusScale)", "[engine][rep]") {
    auto list = generateForType(engine::RepresentationType::Vdw);
    // 必须发射原子球，而非表面网格或线框
    REQUIRE(countCmd(list, engine::Cmd::SphereInst) == 1);
    float r = 0.0f;
    REQUIRE(firstSphereRadius(list, &r));
    // 碳 vdW 半径 1.70；radiusScale=0.5 不影响 VDW（全半径）
    REQUIRE(std::fabs(r - 1.70f) < 1e-3f);
}

TEST_CASE("QuickSurf dispatches to a TriMesh (kDrawTable index 8)", "[engine][rep]") {
    auto list = generateForType(engine::RepresentationType::QuickSurf);
    // 修复前此处为越界空指针；现应正常产出高斯等值面三角网格
    REQUIRE(countCmd(list, engine::Cmd::TriMesh) == 1);
}

TEST_CASE("CPK(Spacefill) uses scaled vdW radius (distinct from Vdw)", "[engine][rep]") {
    auto list = generateForType(engine::RepresentationType::Spacefill);
    REQUIRE(countCmd(list, engine::Cmd::SphereInst) == 1);
    float r = 0.0f;
    REQUIRE(firstSphereRadius(list, &r));
    // radiusScale=0.5 × 碳 vdW 1.70 = 0.85
    REQUIRE(std::fabs(r - 0.85f) < 1e-3f);
}
