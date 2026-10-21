// =============================================================================
// MolecuLens · tests · io/pdb_parser_tests.cpp —— PDB 解析 + 骨架样式回归
//
// 回归目标（本文件新增的直接触发原因）：
//   旧 PDB 解析器用「残基名」当 residueId 去重键（GLY/ALA/SOL 各一个 id）。
//   Atom::residueId 的语义是**残基索引**，卡通/管状/丝带/新卡通都以
//   (chainId, residueId) 为残基分组键 → 所有 GLY 被折叠成一“个残基”，
//   整条蛋白骨架只剩十几个相距很远的点，再被断链阈值过滤：
//   实测 1UBQ（76 残基）卡通只生成 1 个网格对象、管状只有 2 段圆柱，
//   视口里几乎完全看不见（“不能渲染”）。
//
// 本测试用自造 PDB（重复残基名 + 多残基）断言：
//   1. 每个残基出现一次就获得唯一 residueId，且残基名可回查；
//   2. 同一条链共享 chainId；
//   3. Tube 生成的圆柱数 = 残基数 - 1（旧实现远小于该值）；
//   4. Cartoon 网格的 X 跨度覆盖整条链（旧实现塌缩在一个残基附近）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"
#include "engine/rep/representation.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"

using namespace moleculens;

namespace {

constexpr int kResidues = 6;
constexpr float kCaStep = 3.8f;   // 相邻 CA 间距（Å），与真实蛋白一致

// 造一个最小 PDB：每残基 N/CA/C 三个原子，残基名在 GLY/ALA 间交替
// （残基名重复正是旧解析器把不同残基折叠到一起的条件）。
// 列宽严格按 PDB 规范（原子名 13-16、残基名 18-20、链 22、残基号 23-26、
// iCode 27、坐标 31-54、元素 77-78）。
std::filesystem::path writeFixture() {
    const auto path =
        std::filesystem::temp_directory_path() / "moleculens_pdb_regression.pdb";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << "HEADER    MO LECULENS TEST FIXTURE\n";
    int serial = 0;
    for (int r = 0; r < kResidues; ++r) {
        const char* resName = (r % 2 == 0) ? "GLY" : "ALA";
        const char* names[3] = {" N  ", " CA ", " C  "};
        const char* elements[3] = {" N", " C", " C"};
        // N(x-1.2) / CA(x) / C(x+1.2)：让每个原子都在同一残基附近
        const float offsets[3] = {-1.2f, 0.0f, 1.2f};
        for (int k = 0; k < 3; ++k) {
            const float x = 10.0f + kCaStep * static_cast<float>(r) + offsets[k];
            char line[100] = {};
            std::snprintf(
                line, sizeof(line),
                "ATOM  %5d %-4s%1s%3s %1s%4d%1s   %8.3f%8.3f%8.3f%6.2f%6.2f          %2s",
                ++serial, names[k], " ", resName, "A", r + 1, " ", x, 10.0f, 10.0f,
                1.0, 0.0, elements[k]);
            out << line << "\n";
        }
    }
    out << "END\n";
    return path;
}

// 生成某表示的完整命令流（与 Renderer::rebuildCommandList 的单实例路径等价）
engine::SceneCommandList generate(const entity::SceneStructure& scene,
                                  engine::RepresentationType type) {
    engine::Representation rep;
    rep.type = type;

    entity::FrameData frame;
    frame.positions = &scene.coordsSoA().positions;
    std::vector<bool> mask(scene.coordsSoA().atomIds.size(), true);

    engine::SceneCommandList out;
    engine::Representer representer;
    representer.generate(rep, scene, scene.instances().front(), 0, frame, 0, mask, out);
    return out;
}

size_t countInstances(const engine::SceneCommandList& list, engine::Cmd code,
                      size_t instanceSize) {
    size_t n = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == code) {
            n += it.payloadSize() / instanceSize;
        }
    }
    return n;
}

// 取命令流里所有 TriMesh 顶点的世界坐标（用于断言网格跨度）
std::vector<glm::vec3> meshVertices(const engine::SceneCommandList& list) {
    std::vector<glm::vec3> out;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() != engine::Cmd::TriMesh) {
            continue;
        }
        if (it.payloadSize() < sizeof(engine::TriMeshHeader)) {
            continue;
        }
        engine::TriMeshHeader hdr{};
        std::memcpy(&hdr, it.payload(), sizeof(hdr));
        if (hdr.vertexCount > 1000000u ||
            sizeof(engine::TriMeshHeader) +
                    static_cast<size_t>(hdr.vertexCount) * sizeof(engine::TriMeshVertex) >
                it.payloadSize()) {
            continue;
        }
        std::vector<engine::TriMeshVertex> verts(hdr.vertexCount);
        std::memcpy(verts.data(), it.payload() + sizeof(engine::TriMeshHeader),
                    static_cast<size_t>(hdr.vertexCount) * sizeof(engine::TriMeshVertex));
        for (const auto& v : verts) {
            out.emplace_back(v.pos.x, v.pos.y, v.pos.z);
        }
    }
    return out;
}

} // namespace

TEST_CASE("PDB parser assigns one residueId per residue occurrence",
          "[io][pdb][regression]") {
    const std::filesystem::path fixture = writeFixture();
    auto parsed = entity::ParserRegistry::defaultRegistry().parseFile(fixture, {});
    REQUIRE(static_cast<bool>(parsed));
    entity::SceneStructure& scene = *parsed->scene;
    REQUIRE(scene.instances().size() == 1u);
    REQUIRE(scene.structures().size() == 1u);
    const entity::MoleculeStructure& ms = scene.structures()[0];
    REQUIRE(ms.elements.size() == static_cast<size_t>(kResidues * 3));

    // chainId 全链一致
    const int32_t chain = ms.atoms[0].chainId;
    REQUIRE(chain >= 0);
    for (const auto& a : ms.atoms) {
        REQUIRE(a.chainId == chain);
    }

    // 每个残基唯一 id + 残基名可回查
    std::set<int32_t> ids;
    for (int r = 0; r < kResidues; ++r) {
        const int32_t rid = ms.atoms[static_cast<size_t>(r) * 3].residueId;
        REQUIRE(rid >= 0);
        ids.insert(rid);
        const char* expected = (r % 2 == 0) ? "GLY" : "ALA";
        REQUIRE(std::string(scene.residueName(rid)) == expected);
    }
    REQUIRE(ids.size() == static_cast<size_t>(kResidues));

    std::filesystem::remove(fixture);
}

TEST_CASE("Backbone styles span the whole chain after PDB residueId fix",
          "[engine][rep][pdb][regression]") {
    const std::filesystem::path fixture = writeFixture();
    auto parsed = entity::ParserRegistry::defaultRegistry().parseFile(fixture, {});
    REQUIRE(static_cast<bool>(parsed));
    const entity::SceneStructure& scene = *parsed->scene;

    // Tube：每对相邻 CA 一段圆柱 + 每段起点一个端帽球 + 末点帽
    {
        const auto list = generate(scene, engine::RepresentationType::Tube);
        const size_t cylinders =
            countInstances(list, engine::Cmd::CylinderInst, sizeof(engine::CylinderInst));
        const size_t spheres =
            countInstances(list, engine::Cmd::SphereInst, sizeof(engine::SphereInst));
        INFO("tube cylinders=" << cylinders << " spheres=" << spheres);
        // 旧实现（残基名当 id）在此夹具上只能得到 0 段圆柱
        REQUIRE(cylinders == static_cast<size_t>(kResidues - 1));
        REQUIRE(spheres == static_cast<size_t>(kResidues));
    }

    // Ribbon：每对相邻 CA 一根细圆柱，无端帽
    {
        const auto list = generate(scene, engine::RepresentationType::Ribbon);
        REQUIRE(countInstances(list, engine::Cmd::CylinderInst,
                               sizeof(engine::CylinderInst)) ==
                static_cast<size_t>(kResidues - 1));
    }

    // Cartoon：单链一个 TriMesh，X 跨度覆盖整条链（旧实现塌缩到 ~1 Å）
    {
        const auto list = generate(scene, engine::RepresentationType::Cartoon);
        const auto verts = meshVertices(list);
        REQUIRE(verts.size() > 100);
        float lo = verts.front().x, hi = verts.front().x;
        for (const auto& v : verts) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        }
        const float expectedSpan = kCaStep * static_cast<float>(kResidues - 1);
        INFO("cartoon x span=" << (hi - lo) << " expected>=" << expectedSpan);
        REQUIRE(hi - lo >= expectedSpan - 2.0f);
    }

    std::filesystem::remove(fixture);
}

// =============================================================================
// 文件对话框过滤器与解析器必须同源
//
// 用户报「打开文件对话框识别不到可打开文件，目标文件不显示」。除了 ANSI 对话框
// 会把中文路径截断，另一个成因是过滤器漏了扩展名（.ent/.mol/.cub 能解析却不在
// 过滤器里）。本用例把"注册表里到底支持哪些扩展名"钉死：UI 从
// ParserRegistry::allExtensions() 生成过滤器即可，不必再抄一份清单。
// =============================================================================
TEST_CASE("ParserRegistry::allExtensions lists every registered extension",
          "[io][registry]") {
    const auto exts = entity::ParserRegistry::defaultRegistry().allExtensions();
    const std::set<std::string> got(exts.begin(), exts.end());
    const std::set<std::string> expected = {"pdb", "ent", "gro", "sdf", "mol", "cube",
                                            "cub"};
    REQUIRE(got == expected);
    // 去重：同一扩展名不能在列表里出现两次
    REQUIRE(exts.size() == got.size());
}

