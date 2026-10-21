// =============================================================================
// MolecuLens · tests · engine/render/coord_replay_tests.cpp —— 帧增量上传（P4）
// 规格来源：主方案 §5.4 / §5.8.2（脏位域 → 重建粒度）；plan §6.3.4。
//
// 校验目标：帧切（仅 DIRTY_COORDS）时 `Renderer::prepare()` 走**坐标重放**——
// 只把坐标类命令的载荷写回原偏移，不重建整条命令流。核心不变量：
//        重放结果必须与"从头全量重建"逐字节一致。
// 该不变量同时覆盖了池布局、命令序列、命令头与 16B 对齐填充。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/rep/representation.h"
#include "engine/render/renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::entity;

namespace {

// 两原子 + 一条键：同时覆盖 SphereInst 与 CylinderInst 两种坐标命令
struct Fixture {
    SceneStructure scene;
    uint32_t instIdx = 0;
    engine::Representation rep;
    std::vector<engine::Representation> reps;

    Fixture() {
        MoleculeStructure ms;
        for (int i = 0; i < 2; ++i) {
            Atom a;
            a.element = ElementType::Carbon;
            ms.atoms.push_back(a);
            ms.elements.push_back(ElementType::Carbon);
            ms.atomNameIds.push_back(0);
        }
        ms.bonds.push_back(Bond{0, 1});
        ms.key = computeStructureKey(ms);
        const uint32_t si = scene.registerStructure(ms);

        auto traj = std::make_shared<Trajectory>();
        Timestep ts;
        ts.positions = {glm::vec3(0.0f), glm::vec3(1.5f, 0.0f, 0.0f)};
        traj->frames.push_back(std::move(ts));
        instIdx = scene.addTrajectoryInstance(si, Transform{}, std::move(traj));

        rep.name = "ballstick";
        rep.type = engine::RepresentationType::BallAndStick;
        reps.assign(1, rep);
    }

    // 把实例坐标整体沿 +X 平移 d（模拟帧切：拓扑不变、坐标变）
    void shiftPositions(float d) {
        std::vector<glm::vec3> world;
        for (const auto& p : scene.instances()[instIdx].trajectory->frames[0].positions) {
            world.push_back(p + glm::vec3(d, 0.0f, 0.0f));
        }
        scene.writeFramePositions(instIdx, world);
    }
};

std::vector<uint8_t> poolBytes(const engine::SceneCommandList& list) {
    std::vector<uint8_t> out;
    engine::ByteSink sink(out);
    engine::SceneCommandList::writeTo(sink, list);
    return out;
}

} // namespace

// =============================================================================
// 回归：命令流 serial 必须**严格单调递增**。
// 历史缺陷（由性能测试发现）：rebuildCommandList 把新建的空列表 move 进来
// （该列表 serial_ = 0）再 bumpSerial() → 每次全量重建的 serial 恒为 1。
// 而后端解码缓存以 (命令流对象, serial) 为键跳过重复解码 → 第二次重建后误判
// "缓存命中"，继续用上一次的绘制列表（往往还是空场景的）。
// 症状极具误导性：**打开文件后界面一切正常、视口却始终空白**，且日志里 serial
// 一直是 1，看不出是缓存键碰撞。
// =============================================================================
TEST_CASE("Command-list serial increases monotonically across rebuilds", "[engine][render]") {
    Fixture f;
    engine::Renderer renderer;
    renderer.setScene(&f.scene);
    renderer.bindRepresentations(&f.reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    const uint64_t s1 = renderer.commandSerial();
    REQUIRE(s1 > 0);

    // 第二次全量重建（表示修订号变化 → DIRTY_REPRESENTATION → 整条重建）
    renderer.bindRepresentations(&f.reps, 2);
    renderer.update(0.0f);
    renderer.prepare();
    const uint64_t s2 = renderer.commandSerial();
    REQUIRE(s2 > s1);   // 相等 = 缓存键碰撞 → 后端会拿旧绘制列表

    // 第三次：同样必须继续递增（不能只在"第一次之后"偶然变大）
    renderer.bindRepresentations(&f.reps, 3);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.commandSerial() > s2);
}

TEST_CASE("Frame-coords-only change replays instead of rebuilding", "[engine][render]") {
    Fixture f;
    engine::Renderer renderer;
    renderer.setScene(&f.scene);
    renderer.bindRepresentations(&f.reps, 1);

    // 首次：必须全量重建（尚无重放计划）
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
    const uint64_t serialAfterFull = renderer.commandSerial();
    REQUIRE(serialAfterFull > 0);

    // 帧切：仅 DIRTY_COORDS
    f.shiftPositions(3.0f);
    REQUIRE(f.scene.dirtyBits() == DIRTY_COORDS);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.lastPrepareWasReplay());
    // 坐标段已更新 → serial 递增（后端据此重新解码/上传）
    REQUIRE(renderer.commandSerial() > serialAfterFull);

    // —— 核心不变量：重放结果 == 全量重建结果 ——
    const std::vector<uint8_t> replayed = poolBytes(renderer.commandList());

    // 强制整条重建（结构脏位 → 不走重放）
    f.scene.setDirty(DIRTY_STRUCTURE);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
    REQUIRE(poolBytes(renderer.commandList()) == replayed);
}

TEST_CASE("Coords replay carries the new positions verbatim", "[engine][render]") {
    Fixture f;
    engine::Renderer renderer;
    renderer.setScene(&f.scene);
    renderer.bindRepresentations(&f.reps, 1);
    renderer.update(0.0f);
    renderer.prepare();

    f.shiftPositions(7.25f);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.lastPrepareWasReplay());

    // 首个球心应等于平移后的原子坐标
    engine::CommandIterator it(renderer.commandList());
    bool found = false;
    for (bool has = renderer.commandList().first(it); has;
         has = renderer.commandList().next(it)) {
        if (it.code() == engine::Cmd::SphereInst &&
            it.payloadSize() >= sizeof(engine::SphereInst)) {
            const auto* s = reinterpret_cast<const engine::SphereInst*>(it.payload());
            REQUIRE(s->posRadius.x == 7.25f);
            REQUIRE(s->posRadius.y == 0.0f);
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("Pose change is a coordinate edit, not a topology change", "[engine][render]") {
    // 实例/体系位姿变更只动坐标、不动拓扑（原子数/键数不变）→ 必须走坐标重放，
    // 而不是每帧重建整条命令流（交互拖动的流畅度取决于此）。
    for (int which = 0; which < 2; ++which) {
        Fixture f;
        engine::Renderer renderer;
        renderer.setScene(&f.scene);
        renderer.bindRepresentations(&f.reps, 1);
        renderer.update(0.0f);
        renderer.prepare();
        REQUIRE_FALSE(renderer.lastPrepareWasReplay());

        // 只有 DIRTY_COORDS —— 不含 DIRTY_INSTANCE
        Transform tm;
        tm.translation.x = 4.0f;
        if (which == 0) {
            f.scene.setInstanceTransform(f.instIdx, tm);
        } else {
            f.scene.setSystemTransform(f.scene.currentSystemId(), tm);
        }
        REQUIRE((f.scene.dirtyBits() & DIRTY_COORDS) != 0);
        REQUIRE((f.scene.dirtyBits() & DIRTY_INSTANCE) == 0);

        renderer.update(0.0f);
        renderer.prepare();
        REQUIRE(renderer.lastPrepareWasReplay());

        // 重放结果仍须与全量重建一致
        const std::vector<uint8_t> replayed = poolBytes(renderer.commandList());
        f.scene.setDirty(DIRTY_STRUCTURE);
        renderer.update(0.0f);
        renderer.prepare();
        REQUIRE(poolBytes(renderer.commandList()) == replayed);
    }
}

TEST_CASE("TriMesh-bearing representation refuses replay", "[engine][render]") {
    Fixture f;
    f.reps[0].type = engine::RepresentationType::QuickSurf;   // 发射 TriMesh
    engine::Renderer renderer;
    renderer.setScene(&f.scene);
    renderer.bindRepresentations(&f.reps, 1);

    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.commandList().hasMeshCommands());   // 顶点数随坐标变化 → 不可重放

    f.shiftPositions(1.0f);
    renderer.update(0.0f);
    renderer.prepare();
    // 必须回退整条重建，且结果仍与直接重建一致
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
    const std::vector<uint8_t> rebuilt = poolBytes(renderer.commandList());

    f.scene.setDirty(DIRTY_STRUCTURE);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(poolBytes(renderer.commandList()) == rebuilt);
}

TEST_CASE("Property-only change does not rebuild the command stream", "[engine][render]") {
    Fixture f;
    engine::Renderer renderer;
    renderer.setScene(&f.scene);
    renderer.bindRepresentations(&f.reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    const uint64_t serial = renderer.commandSerial();

    // 背景 / 光照 / 雾 / 全局缩放只进 GlobalUBO —— 命令流不应重建
    f.scene.setDirty(DIRTY_PROPERTIES);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
    REQUIRE(renderer.commandSerial() == serial);
}

TEST_CASE("Structural change supersedes a pending coords-only flag", "[engine][render]") {
    Fixture f;
    engine::Renderer renderer;
    renderer.setScene(&f.scene);
    renderer.bindRepresentations(&f.reps, 1);
    renderer.update(0.0f);
    renderer.prepare();

    // 坐标 + 结构同时脏 → 必须整条重建（不得只看 DIRTY_COORDS 而漏掉结构变更）
    f.shiftPositions(2.0f);
    f.scene.setDirty(DIRTY_STRUCTURE);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
}

TEST_CASE("Replay plan is invalidated by deserialization", "[engine][render]") {
    // 反序列化换掉整块池 → 旧偏移记录必须失效（否则会把载荷写到错误位置）
    engine::SceneCommandList src;
    engine::SphereInst s{};
    src.append(engine::Cmd::SphereInst, &s, sizeof(s));
    REQUIRE(src.replayPlan().empty());   // 未开记录模式 → 无计划

    std::vector<uint8_t> bytes;
    engine::ByteSink sink(bytes);
    engine::SceneCommandList::writeTo(sink, src);

    engine::SceneCommandList dst;
    engine::ByteSource in(bytes.data(), bytes.size());
    REQUIRE(engine::SceneCommandList::readFrom(in, dst));
    REQUIRE_FALSE(dst.beginReplay());   // 无计划 → 拒绝重放
}
