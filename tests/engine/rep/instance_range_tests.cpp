// =============================================================================
// MolecuLens · tests · engine/rep/instance_range_tests.cpp —— 实例级类型范围
// 规格来源：团队计划 2026-10-22 任务 C（"只渲染一半溶剂分子"）
//
// 覆盖：
//   · buildInstanceRangeMask 纯函数语义（类型 token / Fraction / Stride / 边界 /
//     顺序确定性）—— 它是命令流重建的输入，必须与场景实例顺序严格对应；
//   · Renderer::prepare 集成：被过滤的实例不再产生 SphereInst（命令流真的变小）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"
#include "engine/render/renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::engine;
using namespace moleculens::entity;

namespace {

// 一个 1 原子结构 + 6 个实例：4 个 SOL（前 4 个）+ 2 个 PROA（后 2 个）。
// 实例名带 "#" 序号（真实导入形态："SOL #12"），用于覆盖类型 token 提取。
// reverseOrder = true 时实例顺序整体反转，用来验证"按场景顺序"而不是按名字排序。
void buildSolventScene(SceneStructure& scene, bool reverseOrder = false) {
    MoleculeStructure ms;
    Atom a{};
    a.element = ElementType::Carbon;
    ms.atoms.push_back(a);
    ms.elements.push_back(ElementType::Carbon);
    ms.atomNameIds.push_back(scene.strings().intern("C"));
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));

    std::vector<std::string> names = {"SOL #1", "SOL #2", "SOL #3",
                                      "SOL #4", "PROA #1", "PROA #2"};
    if (reverseOrder) {
        std::reverse(names.begin(), names.end());
    }
    for (const std::string& n : names) {
        scene.addTrajectoryInstance(si, Transform{}, traj, UINT32_MAX, n,
                                    n.rfind("SOL", 0) == 0);
    }
}

std::vector<uint8_t> maskFor(const Representation& rep,
                             const std::vector<MoleculeInstance>& instances) {
    std::vector<uint8_t> mask;
    buildInstanceRangeMask(rep, instances, mask);
    return mask;
}

// 被选中实例的下标（升序），便于逐项断言"选了哪几个"
std::vector<size_t> selectedIndices(const std::vector<uint8_t>& mask) {
    std::vector<size_t> out;
    for (size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] != 0) {
            out.push_back(i);
        }
    }
    return out;
}

// Catch2 的 REQUIRE 是宏：花括号初始化列表里的逗号会被当成宏参数分隔符，
// 所以把"期望下标表"作为函数实参传进来（初始化列表在宏外面，不会踩这个坑）。
void checkSelected(const std::vector<uint8_t>& mask, const std::vector<size_t>& want) {
    REQUIRE(selectedIndices(mask) == want);
}

Representation rangeRep(std::string type, Representation::InstanceRangeMode mode,
                        float fraction = 1.0f, uint32_t stride = 1, uint32_t offset = 0) {
    Representation rep;
    rep.rangeType = std::move(type);
    rep.rangeMode = static_cast<uint8_t>(mode);
    rep.rangeFraction = fraction;
    rep.rangeStride = stride;
    rep.rangeOffset = offset;
    return rep;
}

// SphereInst **实例数**（命令可能批量装在一条 SphereInst 里）
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

// 命令流里 RenderMode 的 mode 序列（整体系表面分支会先重置一条）
std::vector<uint8_t> renderModes(const SceneCommandList& list) {
    std::vector<uint8_t> modes;
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == Cmd::RenderMode && it.payloadSize() >= sizeof(RenderModeInst)) {
            RenderModeInst rmi{};
            std::memcpy(&rmi, it.payload(), sizeof(rmi));
            modes.push_back(rmi.mode);
        }
    }
    return modes;
}

} // namespace

// =============================================================================
// 纯函数：Fraction（前 ceil(f*N) 个匹配实例）
// =============================================================================
TEST_CASE("instance range fraction keeps the first half of the matching type",
          "[engine][rep][range]") {
    SceneStructure scene;
    buildSolventScene(scene);
    REQUIRE(scene.instances().size() == 6u);

    // 4 个 SOL / 50% → 前 2 个 SOL（场景下标 0/1），PROA 一律不参与
    const auto half = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 0.5f),
        scene.instances());
    REQUIRE(half.size() == 6u);
    checkSelected(half, {0u, 1u});

    // 25% → ceil(1) = 1 个（不是 0：ceil 语义保证"至少取到比例覆盖的部分"）
    const auto quarter = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 0.25f),
        scene.instances());
    checkSelected(quarter, {0u});

    // 0 → 一个都不渲染；1 → 该类型全部
    const auto none = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 0.0f),
        scene.instances());
    REQUIRE(selectedIndices(none).empty());
    const auto all = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 1.0f),
        scene.instances());
    checkSelected(all, {0u, 1u, 2u, 3u});

    // 类型为空 = 全部实例都算匹配：50% 的 6 个 → 前 3 个（ceil(3.0) = 3）
    const auto anyType = maskFor(
        rangeRep("", Representation::InstanceRangeMode::Fraction, 0.5f),
        scene.instances());
    checkSelected(anyType, {0u, 1u, 2u});

    // 越界比例按 [0,1] 夹取；NaN（损坏文件/未初始化 UI 状态）按"全部"处理：
    // 宁可多画也不能因为一个坏字段把整条表示的实例全过滤成空。
    const auto over = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 2.5f),
        scene.instances());
    checkSelected(over, {0u, 1u, 2u, 3u});
    Representation nanRep =
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 0.5f);
    nanRep.rangeFraction = std::numeric_limits<float>::quiet_NaN();
    const auto nanMask = maskFor(nanRep, scene.instances());
    checkSelected(nanMask, {0u, 1u, 2u, 3u});
}

// =============================================================================
// 纯函数：Stride（匹配位置 p % stride == offset % stride）
// =============================================================================
TEST_CASE("instance range stride honours stride and offset", "[engine][rep][range]") {
    SceneStructure scene;
    buildSolventScene(scene);

    // 每隔 2 个取第 0 个：匹配位置 0/2 → SOL #1 / SOL #3
    const auto even = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Stride, 1.0f, 2, 0),
        scene.instances());
    checkSelected(even, {0u, 2u});

    // 偏移 1：位置 1/3 → SOL #2 / SOL #4
    const auto odd = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Stride, 1.0f, 2, 1),
        scene.instances());
    checkSelected(odd, {1u, 3u});

    // 每 3 个取第 2 个：SOL 匹配位置 0..3 → 只剩位置 2（SOL #3）
    const auto third = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Stride, 1.0f, 3, 2),
        scene.instances());
    checkSelected(third, {2u});

    // offset >= stride 一律折回（控制器/文件都可能给出这种值）
    const auto wrap = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Stride, 1.0f, 2, 3),
        scene.instances());
    checkSelected(wrap, {1u, 3u});

    // stride = 0（损坏文件）按 1 处理 = 全部匹配实例
    const auto zero = maskFor(
        rangeRep("SOL", Representation::InstanceRangeMode::Stride, 1.0f, 0, 0),
        scene.instances());
    checkSelected(zero, {0u, 1u, 2u, 3u});
}

// =============================================================================
// 纯函数：类型过滤 + 边界
// =============================================================================
TEST_CASE("instance range type filter is exact and empty means all",
          "[engine][rep][range]") {
    SceneStructure scene;
    buildSolventScene(scene);
    const auto& insts = scene.instances();

    // 全部模式 + 指定类型：只留 PROA
    const auto proa =
        maskFor(rangeRep("PROA", Representation::InstanceRangeMode::All), insts);
    checkSelected(proa, {4u, 5u});

    // 类型为空 = 不按类型过滤（旧行为）
    const auto all = maskFor(rangeRep("", Representation::InstanceRangeMode::All), insts);
    checkSelected(all, {0u, 1u, 2u, 3u, 4u, 5u});

    // 场景里没有的类型 → 全空（不是"回退成全部"）
    const auto missing =
        maskFor(rangeRep("XYZ", Representation::InstanceRangeMode::All), insts);
    REQUIRE(selectedIndices(missing).empty());

    // 用户输入带空白 / 大小写不同：空白 trim 掉；类型名区分大小写（与实例名一致）
    const auto padded =
        maskFor(rangeRep("  SOL  ", Representation::InstanceRangeMode::All), insts);
    checkSelected(padded, {0u, 1u, 2u, 3u});
    const auto lower =
        maskFor(rangeRep("sol", Representation::InstanceRangeMode::All), insts);
    REQUIRE(selectedIndices(lower).empty());

    // 名字没有 "#" 时整个名字就是类型（导入器给的单分子名）
    SceneStructure plain;
    MoleculeStructure ms;
    Atom a{};
    a.element = ElementType::Carbon;
    ms.atoms.push_back(a);
    ms.elements.push_back(ElementType::Carbon);
    ms.atomNameIds.push_back(plain.strings().intern("C"));
    ms.key = computeStructureKey(ms);
    const uint32_t si = plain.registerStructure(ms);
    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    plain.addTrajectoryInstance(si, Transform{}, traj, UINT32_MAX, "molecule_3");
    const auto noHash = maskFor(
        rangeRep("molecule_3", Representation::InstanceRangeMode::All),
        plain.instances());
    checkSelected(noHash, {0u});
}

// =============================================================================
// 纯函数：确定性 + 跟随场景实例顺序（不是按名字排序）
// =============================================================================
TEST_CASE("instance range mask is deterministic and follows instance order",
          "[engine][rep][range]") {
    SceneStructure scene;
    buildSolventScene(scene);
    const Representation rep =
        rangeRep("SOL", Representation::InstanceRangeMode::Fraction, 0.5f);

    const auto first = maskFor(rep, scene.instances());
    const auto second = maskFor(rep, scene.instances());
    REQUIRE(first == second);   // 纯函数：同一输入逐字节相同

    // 反转实例顺序：50% 的 SOL 仍然是"匹配顺序里的前两个"，
    // 即新顺序下第 3/4 个实例（SOL #4 / SOL #3），证明没有按名字排序。
    SceneStructure reversed;
    buildSolventScene(reversed, /*reverseOrder=*/true);
    REQUIRE(reversed.instances()[0].name == "PROA #2");
    const auto revMask = maskFor(rep, reversed.instances());
    checkSelected(revMask, {2u, 3u});
    REQUIRE(reversed.instances()[2].name == "SOL #4");
    REQUIRE(reversed.instances()[3].name == "SOL #3");
}

// =============================================================================
// 集成：Renderer::prepare 的 SphereInst 数量随类型范围变化
// =============================================================================
TEST_CASE("renderer drops instances outside the type range", "[engine][rep][range]") {
    SceneStructure scene;
    buildSolventScene(scene);
    scene.rebuildCoordsSoA();

    // Vdw = 每个原子一颗球 → 6 个实例各 1 原子 = 6 个 SphereInst。
    std::vector<Representation> reps(1);
    reps[0].name = "vdw";
    reps[0].type = RepresentationType::Vdw;

    Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 6u);

    // 50% 的 SOL → 只剩 2 个实例参与渲染
    reps[0].rangeType = "SOL";
    reps[0].rangeMode = static_cast<uint8_t>(Representation::InstanceRangeMode::Fraction);
    reps[0].rangeFraction = 0.5f;
    renderer.bindRepresentations(&reps, 2);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 2u);

    // 每隔 2 个取第 1 个 PROA → 1 个实例
    reps[0].rangeType = "PROA";
    reps[0].rangeMode = static_cast<uint8_t>(Representation::InstanceRangeMode::Stride);
    reps[0].rangeStride = 2;
    reps[0].rangeOffset = 1;
    renderer.bindRepresentations(&reps, 3);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 1u);

    // 清空范围（旧行为）→ 6 个实例全部回来
    reps[0].rangeType.clear();
    reps[0].rangeMode = static_cast<uint8_t>(Representation::InstanceRangeMode::All);
    renderer.bindRepresentations(&reps, 4);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countSphereInstances(renderer.commandList()) == 6u);
}

// =============================================================================
// 集成：整体系表面按表示自己的 surfaceDrawMode 发射 RenderMode（v2.19）
// 此前该分支被写死 mode 0，用户拿不到"表面绘制模式"；同时它也是
// sctx.instance / sctx.instanceMask 的装配点（Task B 的取色/过滤依赖它）。
// =============================================================================
TEST_CASE("system surface emits the representation draw mode", "[engine][rep][range]") {
    SceneStructure scene;
    buildSolventScene(scene);
    scene.rebuildCoordsSoA();

    std::vector<Representation> reps(1);
    reps[0].name = "system-surface";
    reps[0].type = RepresentationType::Surface;
    reps[0].systemSurface = true;
    reps[0].surfGridSpacing = 0.6f;

    Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);

    // 0 实心 / 1 线框 / 2 半透明 逐档验证（整体系分支先重置一条 RenderMode）
    for (uint8_t drawMode = 0; drawMode <= 2; ++drawMode) {
        reps[0].surfaceDrawMode = drawMode;
        renderer.bindRepresentations(&reps, 10u + drawMode);
        renderer.update(0.0f);
        renderer.prepare();
        const auto modes = renderModes(renderer.commandList());
        REQUIRE(modes.size() == 1u);
        REQUIRE(modes[0] == drawMode);
    }

    // 叠加类型范围：整体系表面仍只发射一次（不会按实例拆成多份），
    // 过滤交给 surface.cpp 通过 sctx.instanceMask 消费。
    reps[0].surfaceDrawMode = 1;
    reps[0].rangeType = "SOL";
    reps[0].rangeMode = static_cast<uint8_t>(Representation::InstanceRangeMode::Fraction);
    reps[0].rangeFraction = 0.5f;
    renderer.bindRepresentations(&reps, 20);
    renderer.update(0.0f);
    renderer.prepare();
    const auto modes = renderModes(renderer.commandList());
    REQUIRE(modes.size() == 1u);
    REQUIRE(modes[0] == 1u);
}
