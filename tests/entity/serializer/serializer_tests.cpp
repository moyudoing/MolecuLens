// =============================================================================
// MolecuLens · tests · entity/serializer/serializer_tests.cpp —— .mlscene round-trip
// 规格来源：主方案 §14.4.4（深度相等/坐标精度/计数一致/CRC 校验/版本不匹配/
//           截断文件/边界）
//
// v2 覆盖重点：v1 只存元素符号与实例位姿，原子名/残基/链/BFactor、体系分组、
// 完整光照与表示参数全部丢失（重新打开会掉色掉分组）。本测试逐项断言这些字段
// 能完整往返，并单独校验命令流段（v1 的实现忽略了 snap.commandStream，永远写空）。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "entity/molecule/molecule.h"
#include "persist/mlscene/mlscene.h"

using namespace moleculens::persist::mlscene;
using namespace moleculens::entity;

namespace {

moleculens::persist::mlscene::SceneSnapshot makeSnapshot() {
    SceneSnapshot snap;
    // 结构：水（含原子级字段 —— v2 新增）
    SceneSnapshot::Structure water;
    {
        SceneSnapshot::Atom o;
        o.name = "O";
        o.element = "O";
        o.residueId = 1;
        o.chainId = 0;
        o.bfactor = 12.5f;
        o.occupancy = 1.0f;
        water.atoms.push_back(o);
        for (int i = 0; i < 2; ++i) {
            SceneSnapshot::Atom h;
            h.name = "H" + std::to_string(i + 1);
            h.element = "H";
            h.residueId = 1;
            h.chainId = 0;
            h.bfactor = 20.0f;
            h.occupancy = 0.5f;
            water.atoms.push_back(h);
        }
    }
    water.bonds = {{0, 1, 1}, {0, 2, 1}};
    snap.structures.push_back(water);
    // 场景级名字池（v2：残基名/链名与领域模型一致地按 id 索引存放）
    snap.chainNames = {"A", "B"};
    snap.residueNames = {"GLY", "SOL"};

    // 体系（v2 新增）
    SceneSnapshot::System sys;
    sys.name = "体系 1";
    sys.visible = false;
    sys.transform.translation = {1.0f, 2.0f, 3.0f};
    sys.transform.rotation = {0.7071f, 0.0f, 0.7071f, 0.0f};
    sys.transform.scale = 2.5f;
    sys.renderMode = 2;                                     // 透明
    snap.systems.push_back(sys);

    SceneSnapshot::Instance inst;
    inst.structureIndex = 0;
    inst.systemIndex = 0;
    inst.name = "water";
    inst.visible = true;
    inst.isSolvent = true;
    inst.transform.translation = {4.0f, 5.0f, 6.0f};
    inst.transform.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    inst.transform.scale = 0.5f;
    // 轨迹改为引用源文件 + 指纹（v2：不再内嵌帧坐标）
    inst.trajectory.sourcePath = "/data/run.xtc";
    inst.trajectory.sourceSize = 4096;
    inst.trajectory.sourceMtime = 1700000000;
    inst.trajectory.frameCount = 100;
    inst.trajectory.dt = 2.0f;
    inst.trajectory.periodic = true;
    snap.instances.push_back(inst);

    SceneSnapshot::Representation rep;
    rep.name = "rep0";
    rep.type = "ballstick";
    rep.selection = "*";
    rep.colorScheme = "element";
    rep.radiusScale = 1.0f;
    rep.bondScale = 1.7f;
    rep.visible = true;
    rep.systemSurface = true;
    rep.surfIsovalue = 0.42f;
    rep.surfGridSpacing = 0.33f;
    rep.ambient = {0.6f, 0.6f, 0.6f, 1.0f};
    rep.diffuse = {0.9f, 0.2f, 0.1f, 1.0f};
    rep.specular = {0.3f, 0.3f, 0.3f, 1.0f};
    rep.shininess = 64.0f;
    rep.metallic = 0.8f;
    rep.roughness = 0.25f;
    rep.opacity = 0.5f;
    snap.representations.push_back(rep);

    // 场景属性（v2 补齐雾与全局光三色）
    snap.bgColor = {0.1f, 0.1f, 0.15f};
    snap.fog = {1.5f, 9.0f};
    snap.fogMode = 2.0f;
    snap.fogDensity = 0.07f;
    snap.ambient = 0.3f;
    snap.scale = 1.0f;
    snap.skyColor = {0.9f, 0.95f, 1.0f};
    snap.groundColor = {0.2f, 0.25f, 0.3f};
    snap.ambientColor = {0.5f, 0.55f, 0.6f};
    snap.units = "angstrom";
    // 相机（v2 补齐 roll/fov/ortho）
    snap.cameraTarget = {0, 0, 0};
    snap.cameraDistance = 40.0f;
    snap.cameraYaw = 0.0f;
    snap.cameraPitch = 0.5f;
    snap.cameraRoll = 0.25f;
    snap.cameraFov = 55.0f;
    snap.cameraOrtho = true;
    // 坐标 SoA（3 原子）
    snap.coordsPositions = {0, 0, 0, 0.958f, 0, 0, -0.24f, 0.93f, 0};
    snap.coordsColors = {1, 0.05f, 0.05f, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    snap.coordsAtomIds = {0, 1, 2};
    // 命令流：persist 按不透明字节处理（序列化由 app 层负责），此处填入可辨识的样本
    snap.commandStream = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    return snap;
}

} // namespace

TEST_CASE("mlscene round-trip preserves all data", "[serializer]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_test.mlscene";
    const auto snap = makeSnapshot();
    auto saveResult = MlSceneSerializer::save(snap, path);
    REQUIRE(saveResult.has_value());

    auto loadResult = MlSceneSerializer::load(path);
    REQUIRE(loadResult.has_value());
    const auto& loaded = loadResult.value();

    // —— 结构：原子级字段（v2 关键补齐）——
    REQUIRE(loaded.structures.size() == snap.structures.size());
    REQUIRE(loaded.structures[0].atoms.size() == snap.structures[0].atoms.size());
    for (size_t a = 0; a < snap.structures[0].atoms.size(); ++a) {
        REQUIRE(loaded.structures[0].atoms[a].name == snap.structures[0].atoms[a].name);
        REQUIRE(loaded.structures[0].atoms[a].element ==
                snap.structures[0].atoms[a].element);
        REQUIRE(loaded.structures[0].atoms[a].residueId ==
                snap.structures[0].atoms[a].residueId);
        REQUIRE(loaded.structures[0].atoms[a].chainId ==
                snap.structures[0].atoms[a].chainId);
        REQUIRE(loaded.structures[0].atoms[a].bfactor ==
                Catch::Approx(snap.structures[0].atoms[a].bfactor));
        REQUIRE(loaded.structures[0].atoms[a].occupancy ==
                Catch::Approx(snap.structures[0].atoms[a].occupancy));
    }
    REQUIRE(loaded.structures[0].bonds == snap.structures[0].bonds);
    // 名字池（原子下标各自指向它们）
    REQUIRE(loaded.chainNames == snap.chainNames);
    REQUIRE(loaded.residueNames == snap.residueNames);

    // —— 体系（v2 新增）——
    REQUIRE(loaded.systems.size() == snap.systems.size());
    REQUIRE(loaded.systems[0].name == snap.systems[0].name);
    REQUIRE(loaded.systems[0].visible == snap.systems[0].visible);
    REQUIRE(loaded.systems[0].renderMode == snap.systems[0].renderMode);
    REQUIRE(loaded.systems[0].transform == snap.systems[0].transform);

    // —— 实例：分组归属、溶剂标记、轨迹源引用 ——
    REQUIRE(loaded.instances.size() == snap.instances.size());
    REQUIRE(loaded.instances[0].name == snap.instances[0].name);
    REQUIRE(loaded.instances[0].visible == snap.instances[0].visible);
    REQUIRE(loaded.instances[0].isSolvent == snap.instances[0].isSolvent);
    REQUIRE(loaded.instances[0].systemIndex == snap.instances[0].systemIndex);
    REQUIRE(loaded.instances[0].transform == snap.instances[0].transform);
    REQUIRE(loaded.instances[0].trajectory.sourcePath ==
            snap.instances[0].trajectory.sourcePath);
    REQUIRE(loaded.instances[0].trajectory.sourceSize ==
            snap.instances[0].trajectory.sourceSize);
    REQUIRE(loaded.instances[0].trajectory.sourceMtime ==
            snap.instances[0].trajectory.sourceMtime);
    REQUIRE(loaded.instances[0].trajectory.frameCount ==
            snap.instances[0].trajectory.frameCount);
    REQUIRE(loaded.instances[0].trajectory.dt ==
            Catch::Approx(snap.instances[0].trajectory.dt));
    REQUIRE(loaded.instances[0].trajectory.periodic ==
            snap.instances[0].trajectory.periodic);

    // —— 表示：全部参数（v1 只存 name/type/selection/colorScheme/radiusScale/visible）——
    REQUIRE(loaded.representations.size() == snap.representations.size());
    const auto& lr = loaded.representations[0];
    const auto& sr = snap.representations[0];
    REQUIRE(lr.type == sr.type);
    REQUIRE(lr.colorScheme == sr.colorScheme);
    REQUIRE(lr.bondScale == Catch::Approx(sr.bondScale));
    REQUIRE(lr.systemSurface == sr.systemSurface);
    REQUIRE(lr.surfIsovalue == Catch::Approx(sr.surfIsovalue));
    REQUIRE(lr.surfGridSpacing == Catch::Approx(sr.surfGridSpacing));
    REQUIRE(lr.ambient == sr.ambient);
    REQUIRE(lr.diffuse == sr.diffuse);
    REQUIRE(lr.specular == sr.specular);
    REQUIRE(lr.shininess == Catch::Approx(sr.shininess));
    REQUIRE(lr.metallic == Catch::Approx(sr.metallic));
    REQUIRE(lr.roughness == Catch::Approx(sr.roughness));
    REQUIRE(lr.opacity == Catch::Approx(sr.opacity));

    // —— 场景属性：雾与全局光三色（v2 新增）——
    REQUIRE(loaded.bgColor == snap.bgColor);
    REQUIRE(loaded.fog == snap.fog);
    REQUIRE(loaded.fogMode == Catch::Approx(snap.fogMode));
    REQUIRE(loaded.fogDensity == Catch::Approx(snap.fogDensity));
    REQUIRE(loaded.ambient == Catch::Approx(snap.ambient));
    REQUIRE(loaded.scale == Catch::Approx(snap.scale));
    REQUIRE(loaded.skyColor == snap.skyColor);
    REQUIRE(loaded.groundColor == snap.groundColor);
    REQUIRE(loaded.ambientColor == snap.ambientColor);

    // —— 相机：roll/fov/ortho（v2 新增）——
    REQUIRE(loaded.cameraDistance == Catch::Approx(snap.cameraDistance));
    REQUIRE(loaded.cameraPitch == Catch::Approx(snap.cameraPitch));
    REQUIRE(loaded.cameraRoll == Catch::Approx(snap.cameraRoll));
    REQUIRE(loaded.cameraFov == Catch::Approx(snap.cameraFov));
    REQUIRE(loaded.cameraOrtho == snap.cameraOrtho);

    // —— 坐标精度 ≤ 1e-4 Å ——
    REQUIRE(loaded.coordsPositions.size() == snap.coordsPositions.size());
    for (size_t i = 0; i < snap.coordsPositions.size(); ++i) {
        REQUIRE(std::fabs(loaded.coordsPositions[i] - snap.coordsPositions[i]) < 1e-4f);
    }
    REQUIRE(loaded.coordsAtomIds == snap.coordsAtomIds);
    REQUIRE(loaded.units == snap.units);
    std::filesystem::remove(path);
}

TEST_CASE("mlscene round-trips the command stream bytes", "[serializer]") {
    // v1 的 save() 在此处新建空 SceneCommandList 喂给 writeTo，
    // **完全忽略 snap.commandStream** —— 命令流永远存不进文件。
    const auto path = std::filesystem::temp_directory_path() / "moleculens_cmd.mlscene";
    auto snap = makeSnapshot();
    snap.commandStream.clear();
    for (int i = 0; i < 257; ++i) {
        snap.commandStream.push_back(static_cast<uint8_t>(i & 0xFF));
    }
    REQUIRE(MlSceneSerializer::save(snap, path).has_value());

    auto loaded = MlSceneSerializer::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->commandStream == snap.commandStream);
    std::filesystem::remove(path);
}

TEST_CASE("mlscene CRC rejects tampered bytes", "[serializer]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_crc.mlscene";
    const auto snap = makeSnapshot();
    REQUIRE(MlSceneSerializer::save(snap, path).has_value());

    // 篡改 1 字节（元数据段内）
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(20);
        char c = 'X';
        fs.write(&c, 1);
    }
    auto result = MlSceneSerializer::load(path);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::CrcError);
    std::filesystem::remove(path);
}

TEST_CASE("mlscene rejects version mismatch", "[serializer]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_ver.mlscene";
    const auto snap = makeSnapshot();
    REQUIRE(MlSceneSerializer::save(snap, path).has_value());
    // 改版本字节（偏移 6-7）
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(6);
        const uint16_t v = 99;
        fs.write(reinterpret_cast<const char*>(&v), 2);
        // 版本改动后 CRC 也失效——重算并回填
        fs.seekp(0, std::ios::end);
        const auto size = fs.tellg();
        std::vector<uint8_t> all(static_cast<size_t>(size));
        fs.seekp(0);
        fs.read(reinterpret_cast<char*>(all.data()), size);
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < all.size() - 4; ++i) {
            crc ^= all[i];
            for (int b = 0; b < 8; ++b) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        crc = ~crc;
        fs.seekp(static_cast<std::streamoff>(size) - 4);
        fs.write(reinterpret_cast<const char*>(&crc), 4);
    }
    auto result = MlSceneSerializer::load(path);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::VersionError);
    std::filesystem::remove(path);
}

TEST_CASE("mlscene rejects truncated file", "[serializer]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_trunc.mlscene";
    const auto snap = makeSnapshot();
    REQUIRE(MlSceneSerializer::save(snap, path).has_value());
    // 截断：仅保留头部 32 字节
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> head(32);
        in.read(reinterpret_cast<char*>(head.data()), 32);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(head.data()), 32);
    }
    auto result = MlSceneSerializer::load(path);
    REQUIRE_FALSE(result.has_value());
    std::filesystem::remove(path);
}

TEST_CASE("mlscene empty scene round-trips", "[serializer]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_empty.mlscene";
    SceneSnapshot snap;
    REQUIRE(MlSceneSerializer::save(snap, path).has_value());
    auto result = MlSceneSerializer::load(path);
    REQUIRE(result.has_value());
    REQUIRE(result->structures.empty());
    REQUIRE(result->instances.empty());
    std::filesystem::remove(path);
}
