// =============================================================================
// MolecuLens · tests · entity/trajectory/multiframe_tests.cpp —— 多帧轨迹端到端
//
// 夹具：data/traj_multiframe.pdb（3 个 MODEL/ENDMDL、4 原子、逐帧沿 +X 平移 1 Å）。
// 覆盖：多帧 PDB 解析 → 轨迹帧数/帧坐标；applyTrajectoryFrame 的随机访问一致性
//       （正向、反向、重复访问同一帧结果必须一致 —— 帧切换的核心契约）。
//
// 说明：二进制轨迹（DCD/XTC）的字节级校验在 dcd_reader_tests.cpp（测试内现场
//       构造字节流）。此处验证"多帧文件 → 场景轨迹 → 坐标段"的完整链路。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"

using namespace moleculens;
using namespace moleculens::entity;

namespace {

constexpr float kTol = 1e-3f;

std::filesystem::path multiframePdb() {
    return std::filesystem::path("F:/MolecuLens/data/traj_multiframe.pdb");
}

// 解析夹具；缺失则跳过（与 gro_parser_tests 同一约定）
SceneStructure parseFixture() {
    if (!std::filesystem::exists(multiframePdb())) {
        SKIP("data/traj_multiframe.pdb 缺失，跳过多帧轨迹验证");
    }
    auto res = ParserRegistry::defaultRegistry().parseFile(multiframePdb(), {});
    REQUIRE(res.has_value());
    REQUIRE(res->scene != nullptr);
    return std::move(*res->scene);
}

} // namespace

TEST_CASE("Multi-model PDB yields a 3-frame trajectory", "[entity][trajectory]") {
    SceneStructure scene = parseFixture();

    REQUIRE(scene.instances().size() == 1u);
    const auto& inst = scene.instances()[0];
    REQUIRE(inst.trajectory != nullptr);
    REQUIRE(inst.trajectory->frames.size() == 3u);
    // 每帧 4 个原子（拓扑跨帧共享）
    for (const auto& f : inst.trajectory->frames) {
        REQUIRE(f.positions.size() == 4u);
    }
    // 坐标段长度 = 原子数
    REQUIRE(scene.coordsSoA().positions.size() == 4u);
}

TEST_CASE("Trajectory frames carry distinct coordinates", "[entity][trajectory]") {
    SceneStructure scene = parseFixture();
    const auto& frames = scene.instances()[0].trajectory->frames;

    // 首帧首个原子在原点；末帧沿 +X 平移 2 Å（第 3 个 model）
    REQUIRE(frames[0].positions[0].x == Catch::Approx(0.0f).margin(kTol));
    REQUIRE(frames[2].positions[0].x == Catch::Approx(2.0f).margin(kTol));
    REQUIRE(frames[0].positions[0].y == Catch::Approx(0.0f).margin(kTol));
    // 中间帧位于两者之间
    REQUIRE(frames[1].positions[0].x == Catch::Approx(1.0f).margin(kTol));
    // 第二个原子（O）比第一个大 1.2 Å，且同样逐帧平移
    REQUIRE(frames[0].positions[1].x == Catch::Approx(1.2f).margin(kTol));
    REQUIRE(frames[2].positions[1].x == Catch::Approx(3.2f).margin(kTol));
}

TEST_CASE("applyTrajectoryFrame is random-access consistent", "[entity][trajectory]") {
    SceneStructure scene = parseFixture();

    // 正向访问
    REQUIRE(scene.applyTrajectoryFrame(0, 0));
    REQUIRE(scene.coordsSoA().positions[0].x == Catch::Approx(0.0f).margin(kTol));
    REQUIRE(scene.applyTrajectoryFrame(0, 2));
    REQUIRE(scene.coordsSoA().positions[0].x == Catch::Approx(2.0f).margin(kTol));

    // 反向跳回第 0 帧：结果必须与首次访问第 0 帧完全一致（无累积/无残留）
    REQUIRE(scene.applyTrajectoryFrame(0, 0));
    REQUIRE(scene.coordsSoA().positions[0].x == Catch::Approx(0.0f).margin(kTol));

    // 重复访问同一帧幂等
    REQUIRE(scene.applyTrajectoryFrame(0, 1));
    const glm::vec3 once = scene.coordsSoA().positions[0];
    REQUIRE(scene.applyTrajectoryFrame(0, 1));
    const glm::vec3 twice = scene.coordsSoA().positions[0];
    REQUIRE(once.x == Catch::Approx(twice.x).margin(kTol));
    REQUIRE(once.y == Catch::Approx(twice.y).margin(kTol));

    // 每次应用都会递增坐标版本（渲染器据此感知坐标段变化）
    const uint64_t v0 = scene.coordsSoA().version;
    REQUIRE(scene.applyTrajectoryFrame(0, 2));
    REQUIRE(scene.coordsSoA().version != v0);

    // 越界：实例下标 / 帧下标 → false 且不改变坐标段
    const glm::vec3 before = scene.coordsSoA().positions[0];
    REQUIRE_FALSE(scene.applyTrajectoryFrame(999u, 0u));
    REQUIRE_FALSE(scene.applyTrajectoryFrame(0u, 999u));
    REQUIRE(scene.coordsSoA().positions[0].x == Catch::Approx(before.x).margin(kTol));
}

TEST_CASE("Frame changes mark DIRTY_COORDS only", "[entity][trajectory]") {
    // 帧切必须只置 DIRTY_COORDS：置 DIRTY_INSTANCE/STRUCTURE 会让渲染器重建整条
    // 命令流（大体系播放卡顿的主因）。
    SceneStructure scene = parseFixture();
    scene.clearDirty();

    REQUIRE(scene.applyTrajectoryFrame(0, 1));
    REQUIRE((scene.dirtyBits() & DIRTY_COORDS) != 0);
    REQUIRE((scene.dirtyBits() & DIRTY_STRUCTURE) == 0);
    REQUIRE((scene.dirtyBits() & DIRTY_INSTANCE) == 0);
    REQUIRE((scene.dirtyBits() & DIRTY_REPRESENTATION) == 0);
}
