// =============================================================================
// MolecuLens · tests · entity/serializer/serializer_tests.cpp —— .mlscene round-trip
// 规格来源：主方案 §14.4.4（深度相等/坐标精度/计数一致/CRC 校验/版本不匹配/
//           截断文件/边界）
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "entity/molecule/molecule.h"
#include "persist/mlscene/mlscene.h"

using namespace moleculens::persist::mlscene;
using namespace moleculens::entity;

namespace {

moleculens::persist::mlscene::SceneSnapshot makeSnapshot() {
    SceneSnapshot snap;
    // 结构：水（2 实例去重语义由场景层保证；此处直接填表）
    SceneSnapshot::Structure water;
    water.elements = {"O", "H", "H"};
    water.bonds = {{0, 1, 1}, {0, 2, 1}};
    snap.structures.push_back(water);
    SceneSnapshot::Instance inst;
    inst.structureIndex = 0;
    inst.name = "water";
    inst.visible = true;
    inst.transform = {1.0f, 0, 0, 0, 1.0f, 0, 0, 0, 1.0f};
    inst.frameCount = 1;
    inst.dt = 2.0f;
    snap.instances.push_back(inst);
    SceneSnapshot::Representation rep;
    rep.name = "rep0";
    rep.type = "ballstick";
    rep.selection = "*";
    rep.colorScheme = "element";
    rep.radiusScale = 1.0f;
    rep.visible = true;
    snap.representations.push_back(rep);
    snap.bgColor = {0.1f, 0.1f, 0.15f};
    snap.ambient = 0.3f;
    snap.scale = 1.0f;
    snap.units = "angstrom";
    snap.cameraTarget = {0, 0, 0};
    snap.cameraDistance = 40.0f;
    snap.cameraYaw = 0.0f;
    snap.cameraPitch = 0.5f;
    // 坐标 SoA（3 原子）
    snap.coordsPositions = {0, 0, 0, 0.958f, 0, 0, -0.24f, 0.93f, 0};
    snap.coordsColors = {1, 0.05f, 0.05f, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    snap.coordsAtomIds = {0, 1, 2};
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

    // 深度相等
    REQUIRE(loaded.structures.size() == snap.structures.size());
    REQUIRE(loaded.structures[0].elements == snap.structures[0].elements);
    REQUIRE(loaded.structures[0].bonds == snap.structures[0].bonds);
    REQUIRE(loaded.instances.size() == snap.instances.size());
    REQUIRE(loaded.instances[0].name == snap.instances[0].name);
    REQUIRE(loaded.instances[0].visible == snap.instances[0].visible);
    REQUIRE(loaded.instances[0].frameCount == snap.instances[0].frameCount);
    REQUIRE(loaded.instances[0].dt == Catch::Approx(snap.instances[0].dt));
    REQUIRE(loaded.representations.size() == snap.representations.size());
    REQUIRE(loaded.representations[0].type == snap.representations[0].type);
    REQUIRE(loaded.representations[0].colorScheme == snap.representations[0].colorScheme);
    // 坐标精度 ≤ 1e-4 Å
    REQUIRE(loaded.coordsPositions.size() == snap.coordsPositions.size());
    for (size_t i = 0; i < snap.coordsPositions.size(); ++i) {
        REQUIRE(std::fabs(loaded.coordsPositions[i] - snap.coordsPositions[i]) < 1e-4f);
    }
    REQUIRE(loaded.coordsAtomIds == snap.coordsAtomIds);
    REQUIRE(loaded.units == snap.units);
    REQUIRE(loaded.cameraDistance == Catch::Approx(snap.cameraDistance));
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
