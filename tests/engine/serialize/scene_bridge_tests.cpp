// =============================================================================
// MolecuLens · tests · engine/serialize/scene_bridge_tests.cpp
// 场景 ↔ .mlscene 快照桥接的往返测试（领域模型 → 快照 → 文件 → 快照 → 领域模型）
//
// 覆盖重点：桥接是"保存/载入场景"功能的唯一实现，且字段映射面很宽
// （原子级细节 / 名字池 / 体系 / 实例位姿 / 表示全参数 / 光照 / 相机 / 命令流），
// 逐项断言比"能存能读"更能定位映射错误。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/render/render_parameter.h"   // 材质库（materialPresetIndexByName）
#include "engine/rep/representation.h"
#include "engine/serialize/scene_bridge.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"
#include "persist/mlscene/scene_serializer.h"

using namespace moleculens;
using namespace moleculens::engine;

namespace {

// Steel 在材质表中的下标（按名字查，不用魔法数字 —— 表重排时本用例自动跟随）
const int kSteelPresetIndex = engine::materialPresetIndexByName("Steel");

// 造一个含"2 原子 + 1 键 + 1 体系 + 1 实例"的场景，原子带残基/链/BFactor
void buildScene(entity::SceneStructure& sc) {
    sc.addChain("A");
    sc.addResidue("GLY");

    entity::MoleculeStructure ms;
    for (int i = 0; i < 2; ++i) {
        const entity::ElementType el =
            (i == 0) ? entity::ElementType::Carbon : entity::ElementType::Oxygen;
        ms.elements.push_back(el);
        entity::Atom at{};
        at.element = el;
        at.occupancy = 100;
        at.bfactor = 1250;      // ×100 → 12.50 Å²
        at.residueId = 0;
        at.chainId = 0;
        ms.atoms.push_back(at);
        ms.atomNameIds.push_back(sc.strings().intern(i == 0 ? "CA" : "O"));
    }
    ms.bonds.push_back(entity::Bond{0, 1, 1});
    const uint32_t si = sc.registerStructure(ms);

    auto traj = std::make_shared<entity::Trajectory>();
    traj->dt = 1.5f;
    traj->periodic = true;
    traj->frames.resize(1);
    traj->frames[0].positions = {glm::vec3(0.0f), glm::vec3(1.2f, 0.0f, 0.0f)};

    const uint32_t sysId = sc.addSystem("体系甲");
    sc.setSystemVisible(sysId, false);
    sc.setSystemRenderMode(sysId, entity::SystemRenderMode::Transparent);

    entity::Transform t;
    t.translation = glm::vec3(7.0f, 8.0f, 9.0f);
    sc.addTrajectoryInstance(si, t, traj, sysId, "实例甲", /*isSolvent=*/true);
    sc.rebuildCoordsSoA();

    auto& p = sc.properties();
    p.bgColor[0] = 0.2f;
    p.skyColor[1] = 0.8f;
    p.groundColor[2] = 0.4f;
    p.ambientColor[0] = 0.6f;
    p.fogMode = 2.0f;
    p.fogDensity = 0.09f;
    p.ambient = 0.55f;
    p.units = entity::Units::Nanometer;
}

// 造表示列表 + 相机 + 命令流，跑一轮"保存 → 载入 → 还原"
struct RoundTrip {
    persist::mlscene::SceneSnapshot snap;
    std::vector<Representation> repsIn;
    OrbitCamera camIn;
    SceneCommandList cmdIn;
};

RoundTrip saveRound(const entity::SceneStructure& sc, const std::filesystem::path& path) {
    RoundTrip rt;
    Representation rep;
    rep.name = "rep0";
    rep.type = RepresentationType::QuickSurf;
    rep.selection = "name CA";
    rep.colorScheme = ColorScheme::BFactor;
    rep.radiusScale = 1.4f;
    rep.bondScale = 1.9f;
    rep.visible = false;
    rep.systemSurface = true;
    rep.surfIsovalue = 0.37f;
    rep.surfGridSpacing = 0.28f;
    // ESP 映射表面参数（v3 新增字段；type 仍取 QuickSurf 以兼容既有断言，
    // 这里只验证字段本身的持久化往返）
    rep.espDensityCubePath = "/data/density.cube";
    rep.espPotentialCubePath = "/data/ESP.cube";
    rep.espIsovalue = 0.0021f;
    rep.espVminKcal = -33.5f;
    rep.espVmaxKcal = 41.25f;
    rep.espAutoRange = false;
    rep.espInvert = true;
    rep.espColormap = 4;
    rep.shininess = 71.0f;
    rep.metallic = 0.65f;
    rep.roughness = 0.21f;
    rep.opacity = 0.44f;
    // 材质预设：取一个非默认项，验证"名字"持久化后能恢复出同一下标
    // （存名字而非下标：材质表增删/重排后旧文件不会静默指向别的材质）
    rep.materialPreset = kSteelPresetIndex;
    rt.repsIn.push_back(rep);

    rt.camIn.target = glm::vec3(1.0f, 2.0f, 3.0f);
    rt.camIn.distance = 33.0f;
    rt.camIn.yaw = 0.6f;
    rt.camIn.pitch = 0.7f;
    rt.camIn.roll = 0.15f;
    rt.camIn.fov = 51.0f;
    rt.camIn.ortho = true;

    // 命令流：塞两条真实命令（SphereInst），验证字节级往返
    SphereInst inst{};
    inst.posRadius = glm::vec4(1.0f, 2.0f, 3.0f, 0.5f);
    rt.cmdIn.append(Cmd::SphereInst, &inst, sizeof(inst));

    SnapshotSource in;
    in.scene = &sc;
    in.representations = &rt.repsIn;
    in.camera = &rt.camIn;
    in.commandStream = &rt.cmdIn;
    in.trajectorySourcePath = "/data/run.xtc";
    in.trajectorySourceSize = 8192;
    in.trajectorySourceMtime = 1700000123;
    rt.snap = makeSnapshot(in);

    REQUIRE(persist::mlscene::MlSceneSerializer::save(rt.snap, path).has_value());
    auto loaded = persist::mlscene::MlSceneSerializer::load(path);
    REQUIRE(loaded.has_value());
    rt.snap = loaded.value();
    return rt;
}

} // namespace

TEST_CASE("scene bridge round-trips the domain model", "[serialize][bridge]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_bridge.mlscene";

    entity::SceneStructure src;
    buildScene(src);
    const RoundTrip rt = saveRound(src, path);

    // 还原到全新场景
    entity::SceneStructure dst;
    std::vector<Representation> repsOut;
    OrbitCamera camOut;
    SceneCommandList cmdOut;
    RestoreTarget target;
    target.scene = &dst;
    target.representations = &repsOut;
    target.camera = &camOut;
    target.commandStream = &cmdOut;
    applySnapshot(rt.snap, target);

    // —— 结构 / 原子级细节 ——
    REQUIRE(dst.structures().size() == src.structures().size());
    const auto& msSrc = src.structures()[0];
    const auto& msDst = dst.structures()[0];
    REQUIRE(msDst.elements.size() == msSrc.elements.size());
    REQUIRE(msDst.bonds.size() == msSrc.bonds.size());
    for (size_t i = 0; i < msSrc.elements.size(); ++i) {
        REQUIRE(msDst.elements[i] == msSrc.elements[i]);
        REQUIRE(msDst.atoms[i].occupancy == msSrc.atoms[i].occupancy);
        REQUIRE(msDst.atoms[i].bfactor == msSrc.atoms[i].bfactor);
        REQUIRE(msDst.atoms[i].residueId == msSrc.atoms[i].residueId);
        REQUIRE(msDst.atoms[i].chainId == msSrc.atoms[i].chainId);
        REQUIRE(std::string(dst.atomName(0, static_cast<uint32_t>(i))) ==
                std::string(src.atomName(0, static_cast<uint32_t>(i))));
    }

    // —— 名字池（按 id 重建，下标须与原表一致）——
    REQUIRE(dst.chainCount() == src.chainCount());
    REQUIRE(dst.residueCount() == src.residueCount());
    REQUIRE(std::string(dst.chainName(0)) == std::string(src.chainName(0)));
    REQUIRE(std::string(dst.residueName(0)) == std::string(src.residueName(0)));

    // —— 体系（含显隐与渲染模式）——
    REQUIRE(dst.systems().size() == src.systems().size());
    REQUIRE(dst.systems()[0].name == src.systems()[0].name);
    REQUIRE(dst.systems()[0].visible == src.systems()[0].visible);
    REQUIRE(dst.systems()[0].renderMode == src.systems()[0].renderMode);

    // —— 实例（分组归属 / 溶剂标记 / 位姿）——
    REQUIRE(dst.instances().size() == src.instances().size());
    const auto& iSrc = src.instances()[0];
    const auto& iDst = dst.instances()[0];
    REQUIRE(iDst.name == iSrc.name);
    REQUIRE(iDst.visible == iSrc.visible);
    REQUIRE(iDst.isSolvent == iSrc.isSolvent);
    REQUIRE(iDst.transform.translation == iSrc.transform.translation);
    REQUIRE(iDst.systemId != UINT32_MAX);
    REQUIRE(dst.systemIndexById(iDst.systemId) != UINT32_MAX);

    // —— 场景属性（含全局光三色与雾）——
    const auto& pSrc = src.properties();
    const auto& pDst = dst.properties();
    REQUIRE(pDst.bgColor[0] == Catch::Approx(pSrc.bgColor[0]));
    REQUIRE(pDst.skyColor[1] == Catch::Approx(pSrc.skyColor[1]));
    REQUIRE(pDst.groundColor[2] == Catch::Approx(pSrc.groundColor[2]));
    REQUIRE(pDst.ambientColor[0] == Catch::Approx(pSrc.ambientColor[0]));
    REQUIRE(pDst.fogMode == Catch::Approx(pSrc.fogMode));
    REQUIRE(pDst.fogDensity == Catch::Approx(pSrc.fogDensity));
    REQUIRE(pDst.ambient == Catch::Approx(pSrc.ambient));
    REQUIRE(pDst.units == pSrc.units);

    // —— 相机（含 roll/fov/ortho）——
    REQUIRE(camOut.distance == Catch::Approx(rt.camIn.distance));
    REQUIRE(camOut.yaw == Catch::Approx(rt.camIn.yaw));
    REQUIRE(camOut.roll == Catch::Approx(rt.camIn.roll));
    REQUIRE(camOut.fov == Catch::Approx(rt.camIn.fov));
    REQUIRE(camOut.ortho == rt.camIn.ortho);
    REQUIRE(camOut.target.x == Catch::Approx(rt.camIn.target.x));

    // —— 表示（全参数）——
    REQUIRE(repsOut.size() == rt.repsIn.size());
    REQUIRE(repsOut[0].type == rt.repsIn[0].type);
    REQUIRE(repsOut[0].selection == rt.repsIn[0].selection);
    REQUIRE(repsOut[0].colorScheme == rt.repsIn[0].colorScheme);
    REQUIRE(repsOut[0].bondScale == Catch::Approx(rt.repsIn[0].bondScale));
    REQUIRE(repsOut[0].systemSurface == rt.repsIn[0].systemSurface);
    REQUIRE(repsOut[0].surfIsovalue == Catch::Approx(rt.repsIn[0].surfIsovalue));
    REQUIRE(repsOut[0].surfGridSpacing == Catch::Approx(rt.repsIn[0].surfGridSpacing));
    // ESP 映射表面参数（v3 字段，.mlscene 可选键，旧文件缺键时取默认值）
    REQUIRE(repsOut[0].espDensityCubePath == rt.repsIn[0].espDensityCubePath);
    REQUIRE(repsOut[0].espPotentialCubePath == rt.repsIn[0].espPotentialCubePath);
    REQUIRE(repsOut[0].espIsovalue == Catch::Approx(rt.repsIn[0].espIsovalue));
    REQUIRE(repsOut[0].espVminKcal == Catch::Approx(rt.repsIn[0].espVminKcal));
    REQUIRE(repsOut[0].espVmaxKcal == Catch::Approx(rt.repsIn[0].espVmaxKcal));
    REQUIRE(repsOut[0].espAutoRange == rt.repsIn[0].espAutoRange);
    REQUIRE(repsOut[0].espInvert == rt.repsIn[0].espInvert);
    REQUIRE(repsOut[0].espColormap == rt.repsIn[0].espColormap);
    REQUIRE(repsOut[0].metallic == Catch::Approx(rt.repsIn[0].metallic));
    REQUIRE(repsOut[0].roughness == Catch::Approx(rt.repsIn[0].roughness));
    REQUIRE(repsOut[0].opacity == Catch::Approx(rt.repsIn[0].opacity));
    // 材质预设：存的是**名字**，载入后应恢复出同一下标（不是"碰巧数值相同"）
    REQUIRE(kSteelPresetIndex >= 0);
    REQUIRE(rt.repsIn[0].materialPreset == kSteelPresetIndex);
    REQUIRE(repsOut[0].materialPreset == kSteelPresetIndex);

    // 回归护栏：材质表本身必须自洽 —— Opaque 与 Representation 的字段默认值逐项
    // 相同（否则"新表示 = Opaque"这一自洽性被破坏，且默认观感会随表变动而漂移）
    {
        const Representation def;
        const engine::MaterialPreset& op = engine::materialPresetAt(0);
        REQUIRE(std::string(op.name) == "Opaque");
        REQUIRE(def.materialPreset == 0);
        for (int c = 0; c < 3; ++c) {
            REQUIRE(def.ambient[c] == Catch::Approx(op.ambient[c]));
            REQUIRE(def.diffuse[c] == Catch::Approx(op.diffuse[c]));
            REQUIRE(def.specular[c] == Catch::Approx(op.specular[c]));
        }
        REQUIRE(def.shininess == Catch::Approx(op.shininess));
        REQUIRE(def.metallic == Catch::Approx(op.metallic));
        REQUIRE(def.roughness == Catch::Approx(op.roughness));
        REQUIRE(def.opacity == Catch::Approx(op.opacity));
    }

    // 材质库：名字唯一且可按名反查（名字是 .mlscene 的稳定键）；未知名字返回 -1
    REQUIRE(engine::kMaterialPresetCount >= 23);   // 参考图列出 23 个材质名
    for (int i = 0; i < engine::kMaterialPresetCount; ++i) {
        const char* ni = engine::materialPresetAt(i).name;
        REQUIRE(ni != nullptr);
        REQUIRE(engine::materialPresetIndexByName(ni) == i);
        for (int j = i + 1; j < engine::kMaterialPresetCount; ++j) {
            REQUIRE(std::string(ni) != engine::materialPresetAt(j).name);
        }
    }
    REQUIRE(engine::materialPresetIndexByName("NoSuchMaterial") == -1);
    REQUIRE(engine::materialPresetIndexByName(nullptr) == -1);
    // 越界下标回退到 Opaque（调用方无需自行判界）
    REQUIRE(std::string(engine::materialPresetAt(-5).name) == "Opaque");
    REQUIRE(std::string(engine::materialPresetAt(9999).name) == "Opaque");

    // —— 命令流：字节级往返（v1 曾完全忽略该字段）——
    const auto srcList = rt.cmdIn;
    REQUIRE(cmdOut.size() == srcList.size());

    std::filesystem::remove(path);
}

TEST_CASE("scene bridge records the trajectory source reference", "[serialize][bridge]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_bridge2.mlscene";
    entity::SceneStructure src;
    buildScene(src);
    const RoundTrip rt = saveRound(src, path);

    // 轨迹帧不内嵌，只记录源文件 + 指纹（供载入后按需重放）
    REQUIRE(rt.snap.instances.size() == 1);
    REQUIRE(rt.snap.instances[0].trajectory.sourcePath == "/data/run.xtc");
    REQUIRE(rt.snap.instances[0].trajectory.sourceSize == 8192u);
    REQUIRE(rt.snap.instances[0].trajectory.sourceMtime == 1700000123);
    REQUIRE(rt.snap.instances[0].trajectory.dt == Catch::Approx(1.5f));
    REQUIRE(rt.snap.instances[0].trajectory.periodic);
    std::filesystem::remove(path);
}

// =============================================================================
// 嵌入式保存：多实例体系共享一条轨迹 → 只内嵌一份，读回后整体系同步播放
// =============================================================================

TEST_CASE("scene bridge embeds a shared system trajectory once", "[serialize][bridge]") {
    const auto path =
        std::filesystem::temp_directory_path() / "moleculens_bridge_embed.mlscene";

    // —— 造一个"2 实例 × 2 原子 = 4 原子"的体系，共享一条 2 帧整体系轨迹 ——
    entity::SceneStructure src;
    src.addChain("A");
    src.addResidue("GLY");
    auto addStructure = [&](entity::ElementType el, const char* n0, const char* n1) {
        entity::MoleculeStructure ms;
        for (const char* nm : {n0, n1}) {
            ms.elements.push_back(el);
            entity::Atom at{};
            at.element = el;
            at.occupancy = 100;
            at.residueId = 0;
            at.chainId = 0;
            ms.atoms.push_back(at);
            ms.atomNameIds.push_back(src.strings().intern(nm));
        }
        ms.bonds.push_back(entity::Bond{0, 1, 1});
        return src.registerStructure(ms);
    };
    // 两条结构必须可区分（registerStructure 会去重）→ 元素不同
    const uint32_t s0 = addStructure(entity::ElementType::Carbon, "C1", "C2");
    const uint32_t s1 = addStructure(entity::ElementType::Oxygen, "O1", "O2");
    const uint32_t sysId = src.addSystem("共享体系");

    auto traj = std::make_shared<entity::Trajectory>();
    traj->dt = 3.0f;
    traj->periodic = true;
    traj->frames.resize(2);
    for (int f = 0; f < 2; ++f) {
        auto& fr = traj->frames[f];
        for (int a = 0; a < 4; ++a) {
            fr.positions.push_back(
                glm::vec3(static_cast<float>(a) + 0.5f * static_cast<float>(f),
                          2.0f * static_cast<float>(a), 1.0f + static_cast<float>(f)));
        }
        fr.box = {40.0f, 0, 0, 0, 40.0f, 0, 0, 0, 40.0f};
    }

    // 先按导入后的状态建场景：两实例同体系，再统一挂整体系轨迹
    auto dummy = std::make_shared<entity::Trajectory>();
    dummy->frames.resize(1);
    src.addTrajectoryInstance(s0, entity::Transform{}, dummy, sysId, "A", false);
    src.addTrajectoryInstance(s1, entity::Transform{}, dummy, sysId, "B", false);
    REQUIRE(src.setSystemTrajectory(sysId, traj));
    REQUIRE(src.instances()[0].trajAtomOffset == 0u);
    REQUIRE(src.instances()[1].trajAtomOffset == 2u);
    src.rebuildCoordsSoA();

    // —— 嵌入式快照：同一 shared_ptr 只内嵌一次 ——
    SnapshotSource in;
    in.scene = &src;
    in.embedTrajectories = true;
    const auto snap = makeSnapshot(in);
    REQUIRE(snap.instances.size() == 2u);
    REQUIRE(snap.embeddedTrajectories.size() == 1u);   // 不是 2 份！
    REQUIRE(snap.embeddedTrajectories[0].frameCount == 2ull);
    REQUIRE(snap.embeddedTrajectories[0].atomCount == 4u);
    REQUIRE(snap.embeddedTrajectories[0].positions.size() == 2u * 4u * 3u);
    REQUIRE(snap.instances[0].trajectory.mode == "embedded");
    REQUIRE(snap.instances[0].trajectory.embeddedIndex == 0);
    REQUIRE(snap.instances[1].trajectory.embeddedIndex == 0);
    REQUIRE(snap.instances[0].trajectory.trajAtomOffset == 0u);
    REQUIRE(snap.instances[1].trajectory.trajAtomOffset == 2u);

    // —— 存盘 → 读回 ——
    persist::mlscene::SaveOptions opts;
    opts.embedTrajectories = true;
    REQUIRE(persist::mlscene::MlSceneSerializer::save(snap, path, opts).has_value());
    auto loaded = persist::mlscene::MlSceneSerializer::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->embeddedTrajectories.size() == 1u);

    // —— 还原到全新场景：共享对象 + 偏移 + 帧数/坐标 ——
    entity::SceneStructure dst;
    RestoreTarget target;
    target.scene = &dst;
    applySnapshot(loaded.value(), target);

    REQUIRE(dst.instances().size() == 2u);
    // 多实例共享：两个实例必须指向**同一条** Trajectory（整体系同步播放的结构保证）
    REQUIRE(dst.instances()[0].trajectory.get() == dst.instances()[1].trajectory.get());
    REQUIRE(dst.instances()[0].trajectory != nullptr);
    REQUIRE(dst.instances()[0].trajectory->frames.size() == 2u);
    REQUIRE(dst.instances()[0].trajAtomOffset == 0u);
    REQUIRE(dst.instances()[1].trajAtomOffset == 2u);
    // 首帧坐标逐值一致：共享对象里保存的是**整体系**帧坐标（4 原子），
    // 两个实例各自靠 trajAtomOffset 取自己的切片
    for (size_t k = 0; k < 4; ++k) {
        REQUIRE(dst.instances()[0].trajectory->frames[0].positions[k] ==
                traj->frames[0].positions[k]);
    }

    // 切到第 1 帧：坐标段按各自 trajAtomOffset 取切片（体系同步）
    REQUIRE(dst.applyTrajectoryFrame(0, 1));
    const auto& coords = dst.coordsSoA().positions;
    REQUIRE(coords.size() == 4u);
    for (size_t k = 0; k < 2; ++k) {
        REQUIRE(coords[k].x == Catch::Approx(traj->frames[1].positions[k].x));
        REQUIRE(coords[k].y == Catch::Approx(traj->frames[1].positions[k].y));
        REQUIRE(coords[k].z == Catch::Approx(traj->frames[1].positions[k].z));
        REQUIRE(coords[2 + k].x == Catch::Approx(traj->frames[1].positions[2 + k].x));
    }
    std::filesystem::remove(path);
}
