// =============================================================================
// MolecuLens · tests · engine/rep/render_mode_tests.cpp —— 体系渲染模式命令
// 规格来源：新一.2 每体系独立渲染模式（实心/线框/透明/点）→ 后端切换 VkPipeline
// 校验：Representer 依据体系渲染模式在命令流中发射 Cmd::RenderMode。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"
#include "engine/render/renderer.h"
#include "engine/style/style_registry.h"
#include "engine/vulkan/backend/decode.h"
#include "engine/vulkan/backend/draw_list.h"   // DrawList / computeDrawRuns
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
// 取 const 引用：CommandIterator 与 SceneCommandList::first/next 本身都是只读的，
// 传 const 即可，调用方不必为"只数一数"而放弃 const。
uint32_t countCmd(const engine::SceneCommandList& list, engine::Cmd code) {
    uint32_t n = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == code) n++;
    }
    return n;
}

// 取首条 SphereInst 的半径（Vdw 全半径 / CPK 缩放半径验证）
bool firstSphereRadius(const engine::SceneCommandList& list, float* outR) {
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

// =============================================================================
// 分派表完整性：16 种内置模式（8 通用：Wireframe/Stick/BallAndStick/Spacefill/
// Surface/Vdw/QuickSurf/Licorice + 4 种骨架：Cartoon/NewCartoon/Tube/Ribbon +
// ESP 映射表面 EspSurface + 平板 Plate + 菱形 Diamond + 点云 DotCloud）
// 逐一必须在编译期静态表中有非空槽位。
// 该用例是"表长与枚举 Count 脱节"这一类回归的**穷举式**防线：新增枚举值却忘了
// 补表，这里立刻失败，而不是等到用户选中该样式时越界取空指针崩溃。
// =============================================================================
TEST_CASE("style table covers every RepresentationType with no gap", "[engine][rep]") {
    using engine::RepresentationType;
    const int count = static_cast<int>(RepresentationType::Count);
    REQUIRE(count == 17);   // 8 通用 + 4 骨架 + ESP + 平板 + 菱形 + 点云 + 多面体
    for (int i = 0; i < count; ++i) {
        const auto t = static_cast<RepresentationType>(i);
        INFO("RepresentationType index " << i);
        REQUIRE(engine::staticStyleFor(t) != nullptr);
        REQUIRE(engine::styleFor(t) != nullptr);
        // 无运行时覆盖时 styleFor 必须回退到编译期表（二者同值）
        REQUIRE(engine::styleFor(t) == engine::staticStyleFor(t));
        // 中文显示名必须存在（styles_tool 下拉框直接用；"未知" = 漏注册）
        const char* zh = engine::representationTypeZh(t);
        REQUIRE(zh != nullptr);
        REQUIRE(std::strcmp(zh, "未知") != 0);
    }
    // 越界（Count 自身）必须返回空，而不是取到相邻内存里的函数指针
    REQUIRE(engine::staticStyleFor(RepresentationType::Count) == nullptr);
    REQUIRE(engine::styleFor(RepresentationType::Count) == nullptr);
}

namespace {

// 几何类命令计数（球/柱/线/网格）——不含 MaterialBlock/RenderMode 等状态命令
uint32_t countGeometry(const engine::SceneCommandList& list) {
    uint32_t n = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        switch (it.code()) {
            case engine::Cmd::SphereInst:
            case engine::Cmd::CylinderInst:
            case engine::Cmd::LineInst:
            case engine::Cmd::TriMesh:
                ++n;
                break;
            default:
                break;
        }
    }
    return n;
}

// 5 残基 × (N/C/CA) + 一条 N0–C1 键的夹具，一份即可覆盖全部模式：
//   · 球/棍/线/甘草棍 ← 那条键
//   · 卡通/新卡通/管/丝带 ← 5 个 CA（新卡通要求链长 ≥ 4）
//   · 表面/QuickSurf ← 全部原子
// 每残基三元组的排布让 CA 落在全局下标 2,5,8,11,14（**非 0 起**）——
// 这正是下方"下标 vs 序号"回归用例能测出缺陷的前提。
engine::SceneCommandList generateForAllModes(engine::RepresentationType type) {
    SceneStructure scene;
    MoleculeStructure ms;
    for (int r = 0; r < 5; ++r) {
        const char* names[3] = {"N", "C", "CA"};
        for (int k = 0; k < 3; ++k) {
            Atom a;
            a.element = (k == 0) ? ElementType::Nitrogen : ElementType::Carbon;
            a.chainId = 0;
            a.residueId = r;
            ms.atoms.push_back(a);
            ms.elements.push_back(a.element);
            ms.atomNameIds.push_back(scene.strings().intern(names[k]));
        }
    }
    ms.bonds.push_back(Bond{0, 1});   // 让"键驱动"的样式有几何可画
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    for (int r = 0; r < 5; ++r) {
        const float x = 3.8f * static_cast<float>(r);      // CA 间距 3.8 Å
        ts.positions.emplace_back(x, 0.0f, 0.0f);          // N
        ts.positions.emplace_back(x + 1.0f, 0.0f, 0.0f);   // C
        ts.positions.emplace_back(x, 1.0f, 0.0f);          // CA
    }
    traj->frames.push_back(std::move(ts));
    const uint32_t instIdx =
        scene.addTrajectoryInstance(si, Transform{}, std::move(traj));

    engine::Representation rep;
    rep.type = type;

    entity::FrameData frame;
    frame.positions = &scene.coordsSoA().positions;
    std::vector<bool> mask(scene.coordsSoA().atomIds.size(), true);

    engine::SceneCommandList out;
    engine::Representer r;
    r.generate(rep, scene, scene.instances()[instIdx], instIdx, frame, 0, mask, out);
    return out;
}

} // namespace

TEST_CASE("every RepresentationType generates a command stream", "[engine][rep]") {
    using engine::RepresentationType;
    for (int i = 0; i < static_cast<int>(RepresentationType::Count); ++i) {
        const auto t = static_cast<RepresentationType>(i);
        INFO("RepresentationType index " << i << " ("
             << engine::representationTypeZh(t) << ")");
        const auto list = generateForAllModes(t);
        // 每条表示都必然发射状态命令（Representer::generate 无条件发射）。
        // DotCloud 额外发"切到点模式 + 恢复"两条（3 条），其余样式恰好 1 条。
        const uint32_t renderModeCmds = countCmd(list, engine::Cmd::RenderMode);
        if (t == RepresentationType::DotCloud) {
            REQUIRE(renderModeCmds == 3);
        } else {
            REQUIRE(renderModeCmds == 1);
        }
        REQUIRE(countCmd(list, engine::Cmd::MaterialBlock) == 1);
        // EspSurface 依赖外部 cube 文件（默认路径为空）→ 无几何且静默返回；
        // 其余 12 种在本夹具下都必须画出东西。把这条契约写死，将来给 EspSurface
        // 配置默认数据时会在此处失败，从而被提醒更新本用例。
        if (t == RepresentationType::EspSurface) {
            REQUIRE(countGeometry(list) == 0);
        } else {
            REQUIRE(countGeometry(list) > 0);
        }
    }
}

// =============================================================================
// 回归：backbone 类样式的配色/拾取必须用 CA 的**全局原子下标**
// 历史缺陷：tube/ribbon/newcartoon 传的是"链内序号"（0,1,2…），于是元素配色取的
// 是结构里前几个原子的元素，拾取也命中别的原子。渲染外观正常，只有颜色与交互
// 错 —— 不会当场暴露。
// 夹具特意让 CA 落在**非 0 起**的下标上（每残基 N/C/CA 三元组，CA 位于 2,5,8…）：
// 若下标退化为序号，期望值 {2,5,8} 会变成 {0,1,2}，本用例立刻失败。
// =============================================================================
namespace {

struct BackboneFixture {
    SceneStructure scene;
    uint32_t instIdx = 0;
    std::vector<uint32_t> caAtoms;   // CA 的全局原子下标

    explicit BackboneFixture(size_t residues) {
        MoleculeStructure ms;
        for (size_t r = 0; r < residues; ++r) {
            const char* names[3] = {"N", "C", "CA"};
            for (int k = 0; k < 3; ++k) {
                Atom a;
                a.element = (k == 0) ? ElementType::Nitrogen : ElementType::Carbon;
                a.chainId = 0;
                a.residueId = static_cast<int32_t>(r);
                ms.atoms.push_back(a);
                ms.elements.push_back(a.element);
                ms.atomNameIds.push_back(scene.strings().intern(names[k]));
                if (k == 2) {
                    caAtoms.push_back(static_cast<uint32_t>(ms.atoms.size() - 1));
                }
            }
        }
        ms.key = computeStructureKey(ms);
        const uint32_t si = scene.registerStructure(ms);

        auto traj = std::make_shared<Trajectory>();
        Timestep ts;
        for (size_t r = 0; r < residues; ++r) {
            const float x = 3.8f * static_cast<float>(r);   // CA 间距 3.8 Å（螺旋）
            ts.positions.emplace_back(x, 0.0f, 0.0f);           // N
            ts.positions.emplace_back(x + 1.0f, 0.0f, 0.0f);    // C
            ts.positions.emplace_back(x, 1.0f, 0.0f);           // CA
        }
        traj->frames.push_back(std::move(ts));
        instIdx = scene.addTrajectoryInstance(si, Transform{}, std::move(traj));
    }

    size_t atomCount() const { return scene.coordsSoA().atomIds.size(); }

    engine::SceneCommandList run(engine::RepresentationType type,
                                 const std::vector<bool>& mask) const {
        engine::Representation rep;
        rep.type = type;
        entity::FrameData frame;
        frame.positions = &scene.coordsSoA().positions;
        engine::SceneCommandList out;
        engine::Representer r;
        r.generate(rep, scene, scene.instances()[instIdx], instIdx, frame, 0, mask, out);
        return out;
    }
};

// 收集圆柱命令携带的拾取原子下标
std::vector<uint32_t> cylinderPickIds(const engine::SceneCommandList& list) {
    std::vector<uint32_t> ids;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::CylinderInst &&
            it.payloadSize() >= sizeof(engine::CylinderInst)) {
            const auto* c = reinterpret_cast<const engine::CylinderInst*>(it.payload());
            ids.push_back(c->pickId_pad.x);
        }
    }
    return ids;
}

} // namespace

TEST_CASE("backbone styles address the real CA atom index (not chain ordinal)",
          "[engine][rep]") {
    using engine::RepresentationType;
    const BackboneFixture f(5);
    // 夹具前提：CA 不在 0 起 —— 否则"序号 vs 下标"退化为同一个值，测不出缺陷
    REQUIRE(f.caAtoms == std::vector<uint32_t>{2u, 5u, 8u, 11u, 14u});

    std::vector<bool> all(f.atomCount(), true);

    // Tube：5 个 CA → 4 段圆柱，拾取下标应为前 4 个 CA 的真实下标
    {
        std::vector<uint32_t> ids = cylinderPickIds(f.run(RepresentationType::Tube, all));
        std::sort(ids.begin(), ids.end());
        REQUIRE(ids == std::vector<uint32_t>{2u, 5u, 8u, 11u});
    }
    // Ribbon：同一条 CA 路径，每段一根细圆柱
    {
        std::vector<uint32_t> ids =
            cylinderPickIds(f.run(RepresentationType::Ribbon, all));
        std::sort(ids.begin(), ids.end());
        REQUIRE(ids == std::vector<uint32_t>{2u, 5u, 8u, 11u});
    }
    // NewCartoon：每段 16 个采样点，拾取下标取其归属 CA 的真实下标
    {
        std::vector<uint32_t> ids =
            cylinderPickIds(f.run(RepresentationType::NewCartoon, all));
        REQUIRE_FALSE(ids.empty());
        for (uint32_t id : ids) {
            INFO("pick id " << id);
            REQUIRE(std::find(f.caAtoms.begin(), f.caAtoms.end(), id) !=
                    f.caAtoms.end());
        }
    }
}

// 原子子集（Representation::selection → 掩码）对 backbone 类样式同样生效：
// 只选中孤立的一个 CA 时相邻两段都不该画（与 stick 对"键两端都在选区内"一致）
TEST_CASE("backbone styles honour the per-representation atom subset", "[engine][rep]") {
    using engine::RepresentationType;
    const BackboneFixture f(5);

    std::vector<bool> single(f.atomCount(), false);
    single[f.caAtoms[1]] = true;   // 仅中间那个 CA
    {
        const auto list = f.run(RepresentationType::Tube, single);
        REQUIRE(cylinderPickIds(list).empty());
        REQUIRE(countGeometry(list) == 0);
    }

    std::vector<bool> pair(f.atomCount(), false);
    pair[f.caAtoms[0]] = true;
    pair[f.caAtoms[1]] = true;   // 前两个 CA → 恰好一段
    {
        const auto list = f.run(RepresentationType::Tube, pair);
        REQUIRE(cylinderPickIds(list).size() == 1);
    }
}

// =============================================================================
// 三角网格的**超限切块**（appendTriMesh）。
// 历史缺陷：单条 TriMesh 载荷超过 kCmdMaxPayload(16 MiB) 时，
// CommandIterator::readHeader 会置 done_ 结束遍历 → 整条命令流在该命令处被
// **静默截断**：解码出 0 个网格、commandStats 报 0、几何完全不可见，而命令池里
// 明明有几十 MB 数据。实测 md.gro 的整体系 vdW/QuickSurf 表面单块就有 19–36 MB，
// 正好撞上。此用例用合成大网格复现该规模并验证切块后语义不变。
// =============================================================================
TEST_CASE("oversized triangle meshes are chunked, not silently truncated",
          "[engine][rep][batch]") {
    using namespace moleculens::engine;
    // 构造约 30 MB 的网格（每三角形 3 个独立顶点）→ 必须切成多块
    constexpr uint32_t kTris = 220000;
    std::vector<TriMeshVertex> verts(kTris * 3);
    std::vector<uint32_t> indices(kTris * 3);
    for (uint32_t t = 0; t < kTris; ++t) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t i = t * 3 + k;
            verts[i].pos = glm::vec4(static_cast<float>(i), 0.0f, 0.0f, 1.0f);
            verts[i].normal = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
            verts[i].color = glm::vec4(1.0f);
            indices[i] = i;
        }
    }
    const size_t payloadBytes = sizeof(TriMeshHeader) +
                                verts.size() * sizeof(TriMeshVertex) +
                                indices.size() * sizeof(uint32_t);
    REQUIRE(payloadBytes > kCmdMaxPayload);   // 前提：单块确实超限

    SceneCommandList list;
    list.appendTriMesh(verts, indices);

    // ① 每条 TriMesh 命令都不得超过单命令上限（否则解码端会截断）
    uint32_t meshCmds = 0;
    uint64_t totalIdx = 0, totalVerts = 0;
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() != Cmd::TriMesh) {
            continue;
        }
        ++meshCmds;
        REQUIRE(it.payloadSize() <= kCmdMaxPayload);
        const auto* hdr = reinterpret_cast<const TriMeshHeader*>(it.payload());
        totalVerts += hdr->vertexCount;
        totalIdx += hdr->indexCount;
    }
    REQUIRE(meshCmds >= 2);                       // 确实切了块
    REQUIRE(totalIdx == indices.size());          // 索引总数守恒（没丢三角形）

    // ② 解码端能看到全部块，且每块的索引都在**本块自己的**顶点范围内
    //    （切块必须重映射索引；不重映射会让每块引用越界顶点 → 乱画或崩溃）
    DrawList draw;
    decodeCommandList(list, draw);
    REQUIRE(draw.meshes.size() == meshCmds);
    size_t decodedTris = 0;
    bool allIdxInRange = true;
    for (const auto& m : draw.meshes) {
        if (m.idx.size() % 3 != 0) {
            allIdxInRange = false;
        }
        decodedTris += m.idx.size() / 3;
        for (uint32_t i : m.idx) {
            if (i >= m.verts.size()) {
                allIdxInRange = false;
            }
        }
    }
    // 累加后一次性断言：逐索引 REQUIRE 会在这个规模下产生 66 万次断言调用
    REQUIRE(allIdxInRange);
    REQUIRE(decodedTris == kTris);                // 三角形数守恒
}

// =============================================================================
// 批处理：绘制调用数 = (材质, 模式) 连续段数（computeDrawRuns 的规则）。
// 历史缺陷：Representer 每实例发一条 MaterialBlock，解码端每条都占一个新材质槽
// → 段在每个实例边界断开。md.gro（2042 实例）实测单帧 ~4084 次绘制、平均每次
// 仅 3.2 个实例。修法：材质槽去重（decode.cpp），同表示的实例共用一个下标。
// =============================================================================
TEST_CASE("identical materials collapse into one draw run", "[engine][rep][batch]") {
    using namespace moleculens::engine;
    // 2042 个实例、内容完全相同的材质 → 应当只有 1 段（而非 2042 段）
    std::vector<uint32_t> mats(2042, 7);
    std::vector<uint8_t> modes(2042, 0);
    const auto runs = computeDrawRuns(mats, modes, mats.size());
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].count == 2042);

    // 材质或模式变化处必须断开（否则会用错材质/管线）
    std::vector<uint32_t> m2 = {0, 0, 1, 1, 0};
    std::vector<uint8_t> md2 = {0, 0, 0, 0, 0};
    const auto r2 = computeDrawRuns(m2, md2, m2.size());
    REQUIRE(r2.size() == 3);
    REQUIRE(r2[0].mat == 0);
    REQUIRE(r2[0].count == 2);
    REQUIRE(r2[1].mat == 1);
    REQUIRE(r2[2].mat == 0);

    std::vector<uint32_t> m3 = {0, 0, 0};
    std::vector<uint8_t> md3 = {0, 2, 2};   // 模式变化同样必须断开
    const auto r3 = computeDrawRuns(m3, md3, m3.size());
    REQUIRE(r3.size() == 2);
    REQUIRE(r3[0].mode == 0);
    REQUIRE(r3[1].mode == 2);

    // 退化输入：0 个实例 → 0 段
    REQUIRE(computeDrawRuns({}, {}, 0).empty());
    // mat/mode 数组比实例数短 → 缺位按 0 处理（下标 0 处仍是真值 5/1，故在此断开）。
    // 这是**逐下标**边界检查：重构前的等价代码写的是 `mats.empty() ? 0 : mats[i]`，
    // 数组非空但短于实例数时会**越界读** —— 这里顺带修掉了那个隐患。
    const auto r4 = computeDrawRuns({5}, {1}, 3);
    REQUIRE(r4.size() == 2);
    REQUIRE(r4[0].mat == 5);
    REQUIRE(r4[0].count == 1);
    REQUIRE(r4[1].count == 2);
}

// 端到端：同一条命令流里 2042 条**内容相同**的 MaterialBlock 只能占 1 个材质槽，
// 且所有球共用该槽 → 解码后 sphereMat 全相同。这是上面那条规则能生效的前提。
TEST_CASE("decode deduplicates identical material blocks", "[engine][rep][batch]") {
    using namespace moleculens::engine;
    SceneCommandList list;
    // 刻意取一个**与内置默认材质不同**的材质：若与默认相同，去重会把它并进
    // 槽 0（同样正确，但测不到"新增槽后再去重"这条路径）
    MaterialBlock mat;
    mat.ambient = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
    mat.diffuse = glm::vec4(0.3f, 0.35f, 0.4f, 1.0f);
    mat.specular = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    mat.shininess = 96.0f;
    mat.metallic = 0.5f;
    mat.roughness = 0.2f;
    mat.opacity = 1.0f;
    for (int i = 0; i < 2042; ++i) {
        list.append(Cmd::MaterialBlock, &mat, sizeof(mat));   // 每实例一条，内容相同
        SphereInst s;
        s.posRadius = glm::vec4(static_cast<float>(i), 0.0f, 0.0f, 0.5f);
        s.color = glm::vec4(1.0f);
        s.pickId = glm::uvec2(static_cast<uint32_t>(i), 0);
        s.pad = glm::uvec2(0, 0);
        list.append(Cmd::SphereInst, &s, sizeof(s));
    }

    DrawList draw;
    decodeCommandList(list, draw);
    REQUIRE(draw.spheres.size() == 2042);
    // 槽 0 = 内置默认材质，槽 1 = 去重后唯一的那个 → 共 2 个（而非 2043 个）
    REQUIRE(draw.materials.size() == 2);
    // 全部球共用同一材质下标 → 段数为 1（这正是绘制调用数从 2042 降到 1 的原因）
    const auto runs = computeDrawRuns(draw.sphereMat, draw.sphereMode, draw.spheres.size());
    REQUIRE(runs.size() == 1);
}

// =============================================================================
// 多表示叠加（任务 4）：Renderer 对每条**可见**表示各生成一段命令，各自按自己的
// selection 求值掩码 → 同一条命令流里同时含两条表示的几何。
// =============================================================================
TEST_CASE("multiple visible representations overlay into one command stream",
          "[engine][rep]") {
    SceneStructure scene;
    MoleculeStructure ms;
    for (int i = 0; i < 2; ++i) {
        Atom a;
        a.element = ElementType::Carbon;
        a.chainId = 0;
        a.residueId = i;   // resid 0 / 1 —— 供 selection 圈定子集
        ms.atoms.push_back(a);
        ms.elements.push_back(ElementType::Carbon);
        ms.atomNameIds.push_back(scene.strings().intern("CA"));
    }
    ms.bonds.push_back(Bond{0, 1});
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions = {glm::vec3(0.0f), glm::vec3(1.5f, 0.0f, 0.0f)};
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, Transform{}, std::move(traj));

    // 表示 A：全原子的范德华球（2 个球）；表示 B：仅 resid 1 的甘草棍。
    // B 取 Licorice 而非 Stick：Licorice **只发射圆柱、不发端帽球**，
    // 于是"球计数"完全由 A 决定，断言不会被端帽污染。
    std::vector<engine::Representation> reps(2);
    reps[0].name = "vdw-all";
    reps[0].type = engine::RepresentationType::Vdw;
    reps[1].name = "licorice-resid1";
    reps[1].type = engine::RepresentationType::Licorice;
    reps[1].selection = "resid 1";

    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    const auto& list = renderer.commandList();

    // A：2 个全半径球
    REQUIRE(countCmd(list, engine::Cmd::SphereInst) == 2);
    // B：只有 resid 1 的那个原子入选 → 该原子单端不构成键 → 无圆柱。
    //    这正是"逐表示原子子集真的生效"的证据（若 selection 被忽略，
    //    两个原子都在掩码内就会画出 1 根圆柱）。
    REQUIRE(countCmd(list, engine::Cmd::CylinderInst) == 0);
    REQUIRE(countCmd(list, engine::Cmd::MaterialBlock) == 2);   // 两条表示各一份材质

    // 放开子集（全选）→ 同一场景立刻出现那根键，证明上面的 0 来自 selection
    // 而非"渲染器压根没画第二条表示"。
    // 计 2 而非 1：Licorice 对每根键发射**两段**半圆柱（A→中点 / 中点→B，
    // 实现整键双色），这是该样式的既定语义。
    reps[1].selection.clear();
    renderer.bindRepresentations(&reps, 2);   // 修订号变化 → 重建
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countCmd(renderer.commandList(), engine::Cmd::CylinderInst) == 2);

    // 隐藏表示 A → 球消失，而 B 的键仍在（可见性逐表示独立）
    reps[0].visible = false;
    renderer.bindRepresentations(&reps, 3);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countCmd(renderer.commandList(), engine::Cmd::SphereInst) == 0);
    REQUIRE(countCmd(renderer.commandList(), engine::Cmd::CylinderInst) == 2);
}

// =============================================================================
// v2.12 新增表示：菱形 Diamond / 点云 DotCloud / 平板 Plate 的单原子与双原子回归
// =============================================================================
namespace {

// TriMesh 命令里的三角形总数（Diamond 一颗宝石 = 8 面 = 24 索引）
uint32_t countMeshTriangles(const engine::SceneCommandList& list) {
    uint32_t tris = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() != engine::Cmd::TriMesh ||
            it.payloadSize() < sizeof(engine::TriMeshHeader)) {
            continue;
        }
        engine::TriMeshHeader hdr{};
        std::memcpy(&hdr, it.payload(), sizeof(hdr));
        tris += hdr.indexCount / 3;
    }
    return tris;
}

// SphereInst **实例数**（命令可能批量装在一个 SphereInst 命令里）
uint32_t countSphereInstances(const engine::SceneCommandList& list) {
    uint32_t n = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::SphereInst) {
            n += it.payloadSize() / sizeof(engine::SphereInst);
        }
    }
    return n;
}

// 收集 RenderMode 命令的 mode 序列
std::vector<uint8_t> renderModes(const engine::SceneCommandList& list) {
    std::vector<uint8_t> modes;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::RenderMode &&
            it.payloadSize() >= sizeof(engine::RenderModeInst)) {
            engine::RenderModeInst rm{};
            std::memcpy(&rm, it.payload(), sizeof(rm));
            modes.push_back(rm.mode);
        }
    }
    return modes;
}

} // namespace

TEST_CASE("Polyhedron emits one icosahedron per heavy atom",
          "[engine][rep][polyhedron]") {
    BackboneFixture fx(2);   // 6 个重原子
    std::vector<bool> mask(fx.atomCount(), true);
    const auto list = fx.run(engine::RepresentationType::Polyhedron, mask);
    // 20 面/原子 × 6 = 120 三角形
    REQUIRE(countMeshTriangles(list) == 120);
    REQUIRE(countCmd(list, engine::Cmd::SphereInst) == 0);
}

TEST_CASE("Diamond emits one octahedron per heavy atom", "[engine][rep][diamond]") {
    BackboneFixture fx(3);   // 3 残基 × 3 重原子（N/C/CA，无 H）
    std::vector<bool> mask(fx.atomCount(), true);
    const auto list = fx.run(engine::RepresentationType::Diamond, mask);
    // 每个非氢原子一颗八面体 = 8 三角形；9 个重原子 → 72 三角形
    REQUIRE(countMeshTriangles(list) == 72);
    REQUIRE(countCmd(list, engine::Cmd::SphereInst) == 0);
    REQUIRE(countCmd(list, engine::Cmd::CylinderInst) == 0);
}

TEST_CASE("DotCloud switches to point mode and emits one sphere per atom",
          "[engine][rep][dotcloud]") {
    BackboneFixture fx(2);   // 6 原子
    std::vector<bool> mask(fx.atomCount(), true);
    const auto list = fx.run(engine::RepresentationType::DotCloud, mask);
    REQUIRE(countSphereInstances(list) == 6);
    REQUIRE(countCmd(list, engine::Cmd::CylinderInst) == 0);
    const std::vector<uint8_t> modes = renderModes(list);
    // Representer 的体系模式 + 样式内的「点」+ 恢复，共 3 条
    REQUIRE(modes.size() == 3u);
    REQUIRE(modes[0] == 0u);   // 体系实心
    REQUIRE(modes[1] == 3u);   // 临时点模式
    REQUIRE(modes[2] == 0u);   // 恢复体系模式
}

TEST_CASE("Plate falls back to a small tab for 1-2 atom residues",
          "[engine][rep][plate]") {
    // 单原子结构（离子/单原子分子）：旧实现直接跳过 → 画面空白。
    SceneStructure scene;
    MoleculeStructure ms;
    Atom a;
    a.element = ElementType::Carbon;
    a.chainId = -1;
    a.residueId = -1;
    ms.atoms.push_back(a);
    ms.elements.push_back(a.element);
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    const uint32_t inst = scene.addTrajectoryInstance(si, Transform{}, std::move(traj));

    engine::Representation rep;
    rep.type = engine::RepresentationType::Plate;
    entity::FrameData frame;
    frame.positions = &scene.coordsSoA().positions;
    std::vector<bool> mask(scene.coordsSoA().atomIds.size(), true);
    engine::SceneCommandList out;
    engine::Representer r;
    r.generate(rep, scene, scene.instances()[inst], inst, frame, 0, mask, out);
    // 一块方板 = 6 面 × 2 三角形 = 12
    REQUIRE(countMeshTriangles(out) == 12);
}
