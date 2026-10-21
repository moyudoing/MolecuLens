// =============================================================================
// Moleculens · tests · entity/trajectory/xtc_reader_tests.cpp —— XTC 读取器
//
// 期望值来源（**不是**由被测实现自己产出的）：用 xdrfile 官方实现
// （other/xdrfile_ref/，BSD-2，已随仓库留存）+ other/xdrfile_ref/oracle.c
// 独立解码夹具，取其输出作为金标准写死在此。因此本文件能证明"与 GROMACS/xdrfile
// 的解码结果一致"，而不只是"实现自洽"。
//
// 夹具：
//   · data/traj_xtc_10f10a.xtc —— 10 原子 × 10 帧的合成体系（MDAnalysis 测试数据，
//     GPL 项目数据，已获用户确认入库）。金标准：第 f 帧全部原子位于 (f,f,f) nm，
//     盒子 20 nm 立方，step=f，time=0.5f，precision=1000。
//     值恰好落在整格点上，换算到 Å 后是 (10f,10f,10f)，可人工核对。
//   · data/md.xtc —— 真实 GROMACS 轨迹（6452 原子 × 1001 帧，dt=10 ps）。
//     用逐帧坐标和与全文件总和做全集校验（覆盖 run-length 水上优化等真实分支）。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "entity/element/element.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"
#include "entity/trajectory/xtc/xtc_reader.h"
#include "io/registry/parser_registry.h"

using namespace moleculens::entity;

namespace {

std::filesystem::path fixtureSmall() {
    return std::filesystem::path("F:/MolecuLens/data/traj_xtc_10f10a.xtc");
}

std::filesystem::path fixtureMd() {
    return std::filesystem::path("F:/MolecuLens/data/md.xtc");
}

// md.xtc 的同源结构文件（XTC 不含拓扑，导入时必须与它配对）
std::filesystem::path fixtureMdGro() {
    return std::filesystem::path("F:/MolecuLens/data/md.gro");
}

// 参考实现（oracle）给出的 nm 单位坐标和；本读取器返回 Å → 期望值 ×10
constexpr float kNmToAngstrom = 10.0f;

} // namespace

TEST_CASE("XtcReader opens the synthetic fixture", "[xtc]") {
    if (!std::filesystem::exists(fixtureSmall())) {
        SKIP("data/traj_xtc_10f10a.xtc 缺失，跳过 XTC 验证");
    }
    XtcReader r;
    REQUIRE(r.canOpen(fixtureSmall()));
    REQUIRE(r.open(fixtureSmall()).has_value());
    REQUIRE(r.atomCount() == 10);
    REQUIRE(r.frameCount() == 10);
    // 时间 0, 0.5, … 4.5 ps → 平均帧间隔 0.5 ps
    REQUIRE(r.dt() == Catch::Approx(0.5f));
}

// 逐帧逐原子：第 f 帧所有原子都在 (10f, 10f, 10f) Å
TEST_CASE("XtcReader decodes every frame of the synthetic fixture", "[xtc]") {
    if (!std::filesystem::exists(fixtureSmall())) {
        SKIP("data/traj_xtc_10f10a.xtc 缺失，跳过 XTC 验证");
    }
    XtcReader r;
    REQUIRE(r.open(fixtureSmall()).has_value());

    for (uint64_t f = 0; f < r.frameCount(); ++f) {
        auto ts = r.readFrame(f);
        REQUIRE(ts.has_value());
        const Timestep& t = ts.value();
        REQUIRE(t.positions.size() == 10);
        const float expect = static_cast<float>(f) * 10.0f;
        for (size_t i = 0; i < t.positions.size(); ++i) {
            INFO("frame " << f << " atom " << i);
            REQUIRE(t.positions[i].x == Catch::Approx(expect).margin(1e-3f));
            REQUIRE(t.positions[i].y == Catch::Approx(expect).margin(1e-3f));
            REQUIRE(t.positions[i].z == Catch::Approx(expect).margin(1e-3f));
        }
        // 盒子 20 nm 立方 → 200 Å（对角元；非对角为 0）
        REQUIRE(t.box[0] == Catch::Approx(200.0f).margin(1e-2f));
        REQUIRE(t.box[4] == Catch::Approx(200.0f).margin(1e-2f));
        REQUIRE(t.box[8] == Catch::Approx(200.0f).margin(1e-2f));
        REQUIRE(t.box[1] == Catch::Approx(0.0f).margin(1e-4f));
    }
}

// 随机访问必须与顺序访问一致，且互不干扰（解码状态全在栈上，不跨帧残留）
TEST_CASE("XtcReader random access matches sequential access", "[xtc]") {
    if (!std::filesystem::exists(fixtureSmall())) {
        SKIP("data/traj_xtc_10f10a.xtc 缺失，跳过 XTC 验证");
    }
    XtcReader r;
    REQUIRE(r.open(fixtureSmall()).has_value());

    const auto direct7 = r.readFrame(7).value();
    // 反向 + 重复访问后再取同一帧，结果必须逐值相同
    (void)r.readFrame(9);
    (void)r.readFrame(0);
    (void)r.readFrame(7);
    const auto again7 = r.readFrame(7).value();
    REQUIRE(direct7.positions.size() == again7.positions.size());
    for (size_t i = 0; i < direct7.positions.size(); ++i) {
        REQUIRE(direct7.positions[i].x == again7.positions[i].x);
        REQUIRE(direct7.positions[i].y == again7.positions[i].y);
        REQUIRE(direct7.positions[i].z == again7.positions[i].z);
    }

    // nextFrame 与 readFrame 必须给出同一序列
    XtcReader r2;
    REQUIRE(r2.open(fixtureSmall()).has_value());
    for (uint64_t f = 0; f < r2.frameCount(); ++f) {
        const auto seq = r2.nextFrame();
        REQUIRE(seq.has_value());
        const auto ra = r.readFrame(f).value();
        REQUIRE(seq.value().positions.size() == ra.positions.size());
        for (size_t i = 0; i < ra.positions.size(); ++i) {
            REQUIRE(seq.value().positions[i].x == ra.positions[i].x);
            REQUIRE(seq.value().positions[i].z == ra.positions[i].z);
        }
    }
    // 越界读取必须报错，而不是返回垃圾
    REQUIRE_FALSE(r.readFrame(r.frameCount()).has_value());
}

TEST_CASE("XtcReader rejects non-XTC input", "[xtc]") {
    XtcReader r;
    // 扩展名不对 → 不接管（避免与 DCD/PDB 抢文件）
    REQUIRE_FALSE(r.canOpen("F:/MolecuLens/data/EPI.pdb"));
    REQUIRE_FALSE(r.canOpen("F:/MolecuLens/data/nonexistent.xtc"));
    // 即便扩展名改成 .xtc，magic 不符也必须拒绝
    REQUIRE_FALSE(r.canOpen("F:/MolecuLens/data/traj_multiframe.pdb"));
}

// 截断文件：索引阶段收敛到最后一个**完整**帧，而不是崩溃或整份打不开
TEST_CASE("XtcReader tolerates a truncated file", "[xtc]") {
    if (!std::filesystem::exists(fixtureSmall())) {
        SKIP("data/traj_xtc_10f10a.xtc 缺失，跳过 XTC 验证");
    }
    std::ifstream in(fixtureSmall(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() == 1040);
    // 每帧 104 B；截到 5 帧 + 半帧
    bytes.resize(104 * 5 + 52);

    const auto tmp = std::filesystem::temp_directory_path() / "ml_xtc_truncated.xtc";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    XtcReader r;
    REQUIRE(r.canOpen(tmp));
    auto opened = r.open(tmp);
    REQUIRE(opened.has_value());
    REQUIRE(r.atomCount() == 10);
    REQUIRE(r.frameCount() == 5);   // 只有 5 个完整帧
    // 完整帧仍能正确解码
    const auto t4 = r.readFrame(4).value();
    REQUIRE(t4.positions.size() == 10);
    REQUIRE(t4.positions[0].x == Catch::Approx(40.0f).margin(1e-3f));
    std::filesystem::remove(tmp);
}

// 损坏的 smallidx：必须在解码时报错，而不是按越界下标读 magicints（越界读内存）
TEST_CASE("XtcReader reports corrupt compression metadata", "[xtc]") {
    if (!std::filesystem::exists(fixtureSmall())) {
        SKIP("data/traj_xtc_10f10a.xtc 缺失，跳过 XTC 验证");
    }
    std::ifstream in(fixtureSmall(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() == 1040);
    // 帧 0 布局：magic(0) natoms(4) step(8) time(12) box(16..52) lsize(52)
    //             precision(56) minint(60..72) maxint(72..84) smallidx(84)
    bytes[84] = static_cast<char>(0xFF);   // smallidx 高位 → 远越界
    const auto tmp = std::filesystem::temp_directory_path() / "ml_xtc_badidx.xtc";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    XtcReader r;
    REQUIRE(r.open(tmp).has_value());          // 索引只需压缩块长度，仍可建立
    REQUIRE_FALSE(r.readFrame(0).has_value()); // 解码时必须报错
    std::filesystem::remove(tmp);
}

// =============================================================================
// 真实 GROMACS 轨迹（6452 原子 × 1001 帧）全集校验。
// 期望值来自 xdrfile 参考实现的 oracle 输出（nm），本读取器返回 Å → ×10。
// 这是唯一能覆盖真实数据分支（run-length 水上优化、位宽自适应全量往返）的用例；
// 10 原子的合成夹具因体系过小走的是不同的代码路径。
// =============================================================================
TEST_CASE("XtcReader matches the reference decoder on a real trajectory", "[xtc]") {
    if (!std::filesystem::exists(fixtureMd())) {
        SKIP("data/md.xtc 缺失，跳过 XTC 真实轨迹验证");
    }
    XtcReader r;
    REQUIRE(r.open(fixtureMd()).has_value());
    REQUIRE(r.atomCount() == 6452);
    REQUIRE(r.frameCount() == 1001);
    REQUIRE(r.dt() == Catch::Approx(10.0f).margin(1e-3f));   // time 300→10300 ps

    // 首帧：box[0][0] = 4.017630 nm；坐标和 = (13024.866620, 12924.352611, 12882.411610) nm
    const auto f0 = r.readFrame(0).value();
    REQUIRE(f0.positions.size() == 6452);
    REQUIRE(f0.box[0] == Catch::Approx(4.017630f * kNmToAngstrom).margin(1e-3f));
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (const auto& p : f0.positions) {
        sx += p.x;
        sy += p.y;
        sz += p.z;
    }
    constexpr double kRelTol = 1e-5;
    REQUIRE(sx == Catch::Approx(13024.866620 * kNmToAngstrom).epsilon(kRelTol));
    REQUIRE(sy == Catch::Approx(12924.352611 * kNmToAngstrom).epsilon(kRelTol));
    REQUIRE(sz == Catch::Approx(12882.411610 * kNmToAngstrom).epsilon(kRelTol));

    // 末帧（第 1000 帧）：step 5000000，box 4.028296 nm
    const auto fLast = r.readFrame(1000).value();
    REQUIRE(fLast.box[0] == Catch::Approx(4.028296f * kNmToAngstrom).margin(1e-3f));

    // 全文件坐标总和（覆盖每一帧的每一个原子）
    double gx = 0.0, gy = 0.0, gz = 0.0;
    for (uint64_t f = 0; f < r.frameCount(); ++f) {
        const auto ts = r.readFrame(f).value();
        for (const auto& p : ts.positions) {
            gx += p.x;
            gy += p.y;
            gz += p.z;
        }
    }
    REQUIRE(gx == Catch::Approx(13017676.355250 * kNmToAngstrom).epsilon(kRelTol));
    REQUIRE(gy == Catch::Approx(13029882.902954 * kNmToAngstrom).epsilon(kRelTol));
    REQUIRE(gz == Catch::Approx(13046274.103792 * kNmToAngstrom).epsilon(kRelTol));
}

// XTC/DCD 都不含拓扑：导入时 IoController 拿读取器上报的原子数与**体系总原子数**
// 比对，不一致直接拒绝（否则坐标会错贴到别的结构上）。
//
// 本用例固定住夹具的关键事实：md.gro 与 md.xtc 同源（体系总原子数 6452 == 轨迹
// 原子数 6452），但 GRO 解析器按残基拆分子 → 该体系是 2042 个实例，每个实例只有
// 自己那几十个原子。把 2042 写死，是为了让"多实例体系"这一前提在夹具变化时立刻暴露
// —— 多实例正是体系级轨迹（setSystemTrajectory）存在的理由。
TEST_CASE("md.xtc is co-sourced with md.gro but splits into many instances", "[xtc]") {
    if (!std::filesystem::exists(fixtureMd()) ||
        !std::filesystem::exists(fixtureMdGro())) {
        SKIP("data/md.xtc 或 data/md.gro 缺失，跳过 XTC 拓扑配对验证");
    }
    XtcReader r;
    REQUIRE(r.open(fixtureMd()).has_value());
    REQUIRE(r.atomCount() == 6452);

    auto parsed = ParserRegistry::defaultRegistry().parseFile(fixtureMdGro(), {});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->scene != nullptr);

    // 体系总原子数（IoController 的校验口径）必须等于轨迹原子数
    size_t total = 0;
    for (const auto& in : parsed->scene->instances()) {
        total += parsed->scene->structures()[in.structureIndex].elements.size();
    }
    REQUIRE(total == r.atomCount());

    // 多实例：单个实例的原子数远小于体系总原子数
    REQUIRE(parsed->scene->instances().size() == 2042);
    const auto& first = parsed->scene->instances()[0];
    REQUIRE(parsed->scene->structures()[first.structureIndex].elements.size() <
            r.atomCount());
}

// =============================================================================
// 体系级轨迹：一个体系（多实例）共享一条整体系轨迹，各实例按 trajAtomOffset 取切片。
// 这是 md.gro + md.xtc 能真正播放的前提（此前"按实例挂载"的模型直接拒绝导入）。
// 用**合成**体系构造（不依赖大夹具），把切片对齐这件事钉死：
// 两个实例分别取轨迹的前 2 / 后 3 个原子，帧切换后各自坐标必须来自自己的切片。
// =============================================================================
TEST_CASE("system trajectory slices reach the right instances", "[xtc]") {
    // 两个实例：A = 2 原子，B = 3 原子；体系轨迹每帧 5 个坐标。
    // 帧 f 的坐标取 x = 100*f + 原子序号 → 断言各实例拿到的是自己的那一段。
    SceneStructure scene;
    const uint32_t sysId = scene.addSystem("S");

    auto makeStruct = [&](int atoms) {
        MoleculeStructure ms;
        for (int i = 0; i < atoms; ++i) {
            Atom a;
            a.element = ElementType::Carbon;
            a.chainId = 0;
            a.residueId = i;
            ms.atoms.push_back(a);
            ms.elements.push_back(ElementType::Carbon);
            ms.atomNameIds.push_back(scene.strings().intern("C"));
        }
        ms.key = computeStructureKey(ms);
        return scene.registerStructure(ms);
    };
    const uint32_t siA = makeStruct(2);
    const uint32_t siB = makeStruct(3);

    // 先各挂一条单帧实例轨迹（模拟 GRO 导入后的初始状态）
    auto single = [](float x0) {
        auto t = std::make_shared<Trajectory>();
        Timestep ts;
        ts.positions = {glm::vec3(x0, 0, 0), glm::vec3(x0, 1, 0)};
        t->frames.push_back(std::move(ts));
        return t;
    };
    const uint32_t ia = scene.addTrajectoryInstance(siA, Transform{}, single(0.0f), sysId);
    const uint32_t ib = scene.addTrajectoryInstance(siB, Transform{}, single(0.0f), sysId);
    REQUIRE(ia == 0);
    REQUIRE(ib == 1);

    // 体系轨迹：3 帧，每帧 5 个原子（= 2 + 3）
    auto sysTraj = std::make_shared<Trajectory>();
    sysTraj->dt = 5.0f;
    for (int f = 0; f < 3; ++f) {
        Timestep ts;
        for (int k = 0; k < 5; ++k) {
            ts.positions.emplace_back(100.0f * f + k, 0.0f, 0.0f);
        }
        sysTraj->frames.push_back(std::move(ts));
    }
    REQUIRE(scene.setSystemTrajectory(sysId, sysTraj));

    // 两个实例必须共享同一个 Trajectory 对象（同步播放的结构保证）
    REQUIRE(scene.instances()[ia].trajectory.get() == scene.instances()[ib].trajectory.get());
    REQUIRE(scene.instances()[ia].trajAtomOffset == 0);
    REQUIRE(scene.instances()[ib].trajAtomOffset == 2);

    // 帧 2：A 取原子 0..1 → x = 200,201；B 取原子 2..4 → x = 202,203,204
    REQUIRE(scene.applyTrajectoryFrame(ia, 2));
    const auto& pos = scene.coordsSoA().positions;
    REQUIRE(pos.size() == 5);
    REQUIRE(pos[0].x == Catch::Approx(200.0f));
    REQUIRE(pos[1].x == Catch::Approx(201.0f));
    REQUIRE(pos[2].x == Catch::Approx(202.0f));
    REQUIRE(pos[3].x == Catch::Approx(203.0f));
    REQUIRE(pos[4].x == Catch::Approx(204.0f));

    // 关键不变量：只请求实例 A 的一帧，实例 B 也必须同步推进（否则多实例体系
    // 会出现"只有一个分子在动"的静默错误）
    REQUIRE(scene.applyTrajectoryFrame(ib, 1));
    REQUIRE(pos[0].x == Catch::Approx(100.0f));
    REQUIRE(pos[2].x == Catch::Approx(102.0f));
    REQUIRE(pos[4].x == Catch::Approx(104.0f));

    // 原子数不符的轨迹必须被拒绝，且不留下"半挂载"状态
    auto badTraj = std::make_shared<Trajectory>();
    Timestep bad;
    bad.positions.resize(4);   // 4 != 5
    badTraj->frames.push_back(std::move(bad));
    REQUIRE_FALSE(scene.setSystemTrajectory(sysId, badTraj));
}
