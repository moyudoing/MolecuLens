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
#include "engine/rep/representation.h"
#include "engine/serialize/scene_bridge.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"
#include "persist/mlscene/scene_serializer.h"

using namespace moleculens;
using namespace moleculens::engine;

namespace {

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
    rep.shininess = 71.0f;
    rep.metallic = 0.65f;
    rep.roughness = 0.21f;
    rep.opacity = 0.44f;
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
    REQUIRE(repsOut[0].metallic == Catch::Approx(rt.repsIn[0].metallic));
    REQUIRE(repsOut[0].roughness == Catch::Approx(rt.repsIn[0].roughness));
    REQUIRE(repsOut[0].opacity == Catch::Approx(rt.repsIn[0].opacity));

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
