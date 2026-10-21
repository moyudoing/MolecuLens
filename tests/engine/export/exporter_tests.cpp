// =============================================================================
// MolecuLens · tests · engine/export/exporter_tests.cpp —— OBJ / STL / POV 导出
// 校验：三种导出器在"命令流 → 文件"上的结构与计数一致性。
// 重点覆盖重构中修复的缺陷：
//   · OBJ 对象名跨多条同类命令唯一（此前用批内下标 → 重名）
//   · OBJ 顶点/面索引落在已写出的顶点范围内
//   · STL 头部声明的三角形数 == 实际写出的数量
//   · POV 对 TriMesh 有输出（此前表面/卡通/QuickSurf 导出为空）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/export/obj/obj_exporter.h"
#include "engine/export/pov/pov_exporter.h"
#include "engine/export/stl/stl_exporter.h"

using namespace moleculens::engine;

namespace {

SphereInst makeSphere(float x, float y, float z, float r) {
    SphereInst s{};
    s.posRadius = glm::vec4(x, y, z, r);
    s.color = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f);
    s.pickId = glm::uvec2(0, 0);
    s.pad = glm::uvec2(0, 0);
    return s;
}

// 一个含两原子 + 一个三角网格的命令流
SceneCommandList makeList() {
    SceneCommandList list;
    MaterialBlock mat{};
    mat.diffuse = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
    mat.opacity = 1.0f;
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));

    // 两个球刻意分两条命令发出（触发此前的对象重名问题）
    const SphereInst s0 = makeSphere(0.0f, 0.0f, 0.0f, 1.0f);
    const SphereInst s1 = makeSphere(3.0f, 0.0f, 0.0f, 1.0f);
    list.append(Cmd::SphereInst, &s0, sizeof(s0));
    list.append(Cmd::SphereInst, &s1, sizeof(s1));

    // 一个四边形（4 顶点 / 6 索引 = 2 三角形）
    TriMeshVertex verts[4]{};
    verts[0].pos = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    verts[1].pos = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    verts[2].pos = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    verts[3].pos = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    TriMeshHeader hdr{};
    hdr.vertexCount = 4;
    hdr.indexCount = 6;
    std::vector<uint8_t> payload(sizeof(TriMeshHeader) + sizeof(verts) + sizeof(idx));
    std::memcpy(payload.data(), &hdr, sizeof(hdr));
    std::memcpy(payload.data() + sizeof(hdr), verts, sizeof(verts));
    std::memcpy(payload.data() + sizeof(hdr) + sizeof(verts), idx, sizeof(idx));
    list.append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));
    return list;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 统计子串出现次数
size_t countOccurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

TEST_CASE("OBJ export emits unique object names across commands", "[export][obj]") {
    const SceneCommandList list = makeList();
    const std::string path = "test_export.obj";
    REQUIRE(static_cast<bool>(ObjExporter::exportScene(list, path)));

    const std::string obj = readFile(path);
    // 两条 SphereInst 命令 → 两个唯一对象名（此前用批内下标 → 均为 o sphere_0）
    REQUIRE(obj.find("o mesh_1") != std::string::npos);
    REQUIRE(obj.find("o mesh_2") != std::string::npos);
    REQUIRE(obj.find("mtllib test_export.mtl") != std::string::npos);
    // 面行数 = 两球各 1280 三角形（3 级细分）+ 四边形 2 三角形 = 2562
    REQUIRE(countOccurrences(obj, "\nf ") == 2562);
}

TEST_CASE("OBJ face indices stay within emitted vertices", "[export][obj]") {
    const SceneCommandList list = makeList();
    const std::string path = "test_export_idx.obj";
    REQUIRE(static_cast<bool>(ObjExporter::exportScene(list, path)));

    std::istringstream in(readFile(path));
    std::string line;
    size_t vertexCount = 0;
    size_t faceRefs = 0;
    size_t maxIndex = 0;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) {
            ++vertexCount;
        } else if (line.rfind("f ", 0) == 0) {
            std::istringstream fs(line.substr(2));
            size_t i = 0;
            while (fs >> i) {
                maxIndex = std::max(maxIndex, i);
                ++faceRefs;
            }
        }
    }
    REQUIRE(vertexCount > 0);
    REQUIRE(faceRefs > 0);
    // OBJ 顶点从 1 开始编号 → 最大引用不得超过顶点总数
    REQUIRE(maxIndex <= vertexCount);
}

TEST_CASE("STL header triangle count matches bytes written", "[export][stl]") {
    const SceneCommandList list = makeList();
    const std::string path = "test_export.stl";
    REQUIRE(static_cast<bool>(StlExporter::exportScene(list, path)));

    const std::string bytes = readFile(path);
    REQUIRE(bytes.size() >= 84);
    uint32_t triCount = 0;
    std::memcpy(&triCount, bytes.data() + 80, sizeof(triCount));
    // 每个三角形 50 字节（法线 12 + 3 顶点 36 + 属性 2）
    REQUIRE(bytes.size() == 84u + static_cast<size_t>(triCount) * 50u);
    // 球实例也被三角化（此前 STL 只处理 TriMesh → 球棍/空间填充导出空文件）
    // = 两球各 1280 三角形 + 四边形 2 三角形
    REQUIRE(triCount == 2562);
}

TEST_CASE("POV export emits mesh2 for TriMesh", "[export][pov]") {
    const SceneCommandList list = makeList();
    const std::string path = "test_export.pov";
    REQUIRE(static_cast<bool>(PovExporter::exportScene(list, path)));

    const std::string pov = readFile(path);
    REQUIRE(pov.find("sphere {") != std::string::npos);
    // 此前 POV 完全忽略 TriMesh → 表面/卡通/QuickSurf 导出为空
    REQUIRE(pov.find("mesh2 {") != std::string::npos);
    REQUIRE(pov.find("vertex_vectors") != std::string::npos);
    REQUIRE(pov.find("face_indices") != std::string::npos);
}
