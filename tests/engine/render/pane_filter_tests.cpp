// =============================================================================
// MolecuLens · tests · engine/render/pane_filter_tests.cpp —— 多视图显示筛选
//
// 覆盖：
//   · Renderer::setPaneFilter 的实例掩码：只生成所选体系/文件的命令；
//   · 表示掩码：不同视图可以显示不同表示；
//   · 多视图共享同一场景时，先渲染的视图会清掉场景脏位 —— 后渲染的视图用
//     updateWithDirty(快照) 仍必须完成重建（否则副视图永远停在旧画面）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::engine;
using namespace moleculens::entity;

namespace {

// 一个 2 原子分子（两坐标点）挂到指定体系。返回实例下标。
uint32_t addDiatomic(SceneStructure& scene, uint32_t systemId,
                     const std::string& name, float x) {
    MoleculeStructure ms;
    for (int i = 0; i < 2; ++i) {
        Atom a{};
        a.element = ElementType::Carbon;
        ms.atoms.push_back(a);
        ms.elements.push_back(ElementType::Carbon);
        ms.atomNameIds.push_back(scene.strings().intern(i == 0 ? "C1" : "C2"));
    }
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(x, 0.0f, 0.0f);
    ts.positions.emplace_back(x + 1.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    return scene.addTrajectoryInstance(si, Transform{}, traj, systemId, name);
}

// 两个体系：fileA 两个实例（4 原子），fileB 一个实例（2 原子）。
void buildTwoFilesScene(SceneStructure& scene, uint32_t* sysA, uint32_t* sysB) {
    *sysA = scene.addSystem("fileA");
    *sysB = scene.addSystem("fileB");
    addDiatomic(scene, *sysA, "A1", 0.0f);
    addDiatomic(scene, *sysA, "A2", 10.0f);
    addDiatomic(scene, *sysB, "B1", 20.0f);
    scene.rebuildCoordsSoA();
}

uint32_t countSphereInstances(const SceneCommandList& list) {
    uint32_t n = 0;
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == Cmd::SphereInst) {
            n += it.payloadSize() / sizeof(SphereInst);
        }
    }
    return n;
}

std::vector<uint8_t> instanceMaskForSystem(const SceneStructure& scene,
                                           uint32_t systemId) {
    std::vector<uint8_t> mask(scene.instances().size(), 0u);
    for (size_t i = 0; i < scene.instances().size(); ++i) {
        if (scene.instances()[i].systemId == systemId) {
            mask[i] = 1u;
        }
    }
    return mask;
}

} // namespace

TEST_CASE("pane instance filter renders only the selected system",
          "[engine][render][multi-view]") {
    SceneStructure scene;
    uint32_t sysA = 0;
    uint32_t sysB = 0;
    buildTwoFilesScene(scene, &sysA, &sysB);
    REQUIRE(scene.instances().size() == 3u);

    std::vector<Representation> reps(1);
    reps[0].name = "vdw";
    reps[0].type = RepresentationType::Vdw;

    Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 6u);   // 3 实例 × 2 原子

    // 视图 A：只看 fileA 的两个实例
    renderer.setPaneFilter(instanceMaskForSystem(scene, sysA), {});
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 4u);

    // 视图 B：只看 fileB 的一个实例
    renderer.setPaneFilter(instanceMaskForSystem(scene, sysB), {});
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 2u);

    // 清掉筛选（单视图）→ 全部回来
    renderer.clearPaneFilter();
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 6u);
}

TEST_CASE("pane representation filter hides whole representations",
          "[engine][render][multi-view]") {
    SceneStructure scene;
    uint32_t sysA = 0;
    uint32_t sysB = 0;
    buildTwoFilesScene(scene, &sysA, &sysB);

    // 两条 Vdw 表示：全开 = 12 个球。
    std::vector<Representation> reps(2);
    reps[0].name = "rep0";
    reps[0].type = RepresentationType::Vdw;
    reps[1].name = "rep1";
    reps[1].type = RepresentationType::Vdw;

    Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 12u);

    // 视图 2 只显示第二条表示 → 6 个球
    renderer.setPaneFilter({}, {0u, 1u});
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 6u);

    // 两条都隐藏 → 命令流里没有球（不会回退成"全部显示"）
    renderer.setPaneFilter({}, {0u, 0u});
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 0u);
}

TEST_CASE("secondary view rebuilds from an explicit dirty snapshot",
          "[engine][render][multi-view]") {
    SceneStructure scene;
    uint32_t sysA = 0;
    uint32_t sysB = 0;
    buildTwoFilesScene(scene, &sysA, &sysB);

    std::vector<Representation> reps(1);
    reps[0].name = "vdw";
    reps[0].type = RepresentationType::Vdw;

    // 主视图：先渲染并清掉场景脏位（这是多视图里真实发生的顺序）
    Renderer primary;
    primary.setScene(&scene);
    primary.bindRepresentations(&reps, 1);
    primary.prepare();
    REQUIRE(countSphereInstances(primary.commandList()) == 6u);
    REQUIRE(scene.dirtyBits() == 0u);

    // 副视图：筛选 fileB，建立自己的命令流
    Renderer secondary;
    secondary.setScene(&scene);
    secondary.bindRepresentations(&reps, 1);
    secondary.setPaneFilter(instanceMaskForSystem(scene, sysB), {});
    secondary.prepare();
    REQUIRE(countSphereInstances(secondary.commandList()) == 2u);

    // 场景新增一个 fileA 实例：脏位先被主视图消费并清除。
    addDiatomic(scene, sysA, "A3", 30.0f);
    scene.rebuildCoordsSoA();
    scene.setDirty(DIRTY_INSTANCE);
    const uint32_t snapshot = scene.dirtyBits();
    primary.updateWithDirty(0.0f, snapshot);   // 主视图拿到快照并重建
    primary.prepare();
    scene.clearDirty();                        // 主视图 rebuild 后清脏（此处显式等价）
    REQUIRE(scene.dirtyBits() == 0u);
    REQUIRE(countSphereInstances(primary.commandList()) == 8u);

    // 副视图此时已看不到场景脏位；但整帧快照仍应让它重建。
    // fileB 没有变化 → 仍是 2 个球；若没有重建会保留旧命令流（也恰好是 2）——
    // 所以再改一次副视图能看到的内容：把 fileA 的新实例也算进视图。
    std::vector<uint8_t> maskA = instanceMaskForSystem(scene, sysA);
    secondary.setPaneFilter(maskA, {});
    secondary.updateWithDirty(0.0f, snapshot);
    secondary.prepare();
    REQUIRE(countSphereInstances(secondary.commandList()) == 6u);   // 3 实例 × 2 原子
}

TEST_CASE("pane filter change rebuilds without any scene dirty bit",
          "[engine][render][multi-view]") {
    SceneStructure scene;
    uint32_t sysA = 0;
    uint32_t sysB = 0;
    buildTwoFilesScene(scene, &sysA, &sysB);

    std::vector<Representation> reps(1);
    reps[0].name = "vdw";
    reps[0].type = RepresentationType::Vdw;

    Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 6u);
    REQUIRE(scene.dirtyBits() == 0u);

    // 筛选变更本身必须触发重建（不等 DIRTY_*），否则切换视图内容不刷新。
    renderer.setPaneFilter(instanceMaskForSystem(scene, sysA), {});
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 4u);
}
