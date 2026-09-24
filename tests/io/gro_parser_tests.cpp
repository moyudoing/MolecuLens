// =============================================================================
// MolecuLens · tests · io/gro_parser_tests.cpp —— GRO 大体系解析验证
// 规格来源：G4（大体系分子分离与溶剂读取）+ G1（正确距离键搜索）
// 校验：md.gro（6452 原子溶剂化体系）—— 按残基拆分子、溶剂标记、键搜索（水 O–H）
//       （用 data/md.gro，缺失时跳过以复用 demo 数据目录）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "io/gro/gro_parser.h"
#include "io/pdb/pdb_parser.h"
#include "io/registry/parser_registry.h"
#include "io/sdf/sdf_parser.h"

using namespace moleculens;

namespace {

const std::filesystem::path mdGro() { return std::filesystem::path("F:/MolecuLens/data/md.gro"); }
const std::filesystem::path epiSdf() {
    return std::filesystem::path("F:/MolecuLens/data/(+) - Epicatechin.sdf");
}

} // namespace

TEST_CASE("md.gro loads, separates molecules, flags solvent", "[io][gro]") {
    if (!std::filesystem::exists(mdGro())) {
        SKIP("data/md.gro 缺失，跳过大体系验证");
    }
    auto res = entity::ParserRegistry::defaultRegistry().parseFile(mdGro(), {});
    REQUIRE(res);
    entity::SceneStructure& scene = *res->scene;
    // 6452 原子（md.gro 声明）
    const size_t totalAtoms = scene.coordsSoA().positions.size();
    REQUIRE(totalAtoms == 6452u);
    // 按残基连续块拆分子 → 多种分子（API 溶质 + SOL 溶剂）
    REQUIRE(scene.instances().size() > 100);
    // 溶剂标记存在（SOL 残基分子 isSolvent=true）
    bool hasSolvent = false;
    for (const auto& inst : scene.instances()) {
        if (inst.isSolvent) {
            hasSolvent = true;
            break;
        }
    }
    REQUIRE(hasSolvent);
    // 溶剂标记可整体显隐（G4）
    REQUIRE(scene.setSolventVisible(false));
}

TEST_CASE("GRO bond search infers water O-H bonds (distance rule)", "[io][gro][bond]") {
    if (!std::filesystem::exists(mdGro())) {
        SKIP("data/md.gro 缺失，跳过键搜索验证");
    }
    auto res = entity::ParserRegistry::defaultRegistry().parseFile(mdGro(), {});
    REQUIRE(res);
    const auto& scene = *res->scene;
    // 找到第一个水分子（SOL），校验其 O-H 键（距离法应产出 2 个 O-H 键）
    const entity::MoleculeInstance* water = nullptr;
    for (const auto& inst : scene.instances()) {
        if (inst.isSolvent) {
            water = &inst;
            break;
        }
    }
    if (!water) {
        // 无溶剂则退化为跳过（不应发生：md.gro 含 SOL）
        FAIL("md.gro 未检出溶剂分子");
    }
    const auto& ms = scene.structures()[water->structureIndex];
    // 水的元素应为 O + 2H（GRO 元素解析修复：OW→O、HW→H）
    int nO = 0, nH = 0;
    for (const auto el : ms.elements) {
        if (el == entity::ElementType::Oxygen) ++nO;
        if (el == entity::ElementType::Hydrogen) ++nH;
    }
    REQUIRE(nO == 1);
    REQUIRE(nH == 2);
    // 距离法键搜索：水分子应有 2 个 O-H 键（且无 O-O 假键）
    REQUIRE(ms.bonds.size() == 2);
}

TEST_CASE("SDF parser loads and registers in registry", "[io][sdf]") {
    if (!std::filesystem::exists(epiSdf())) {
        SKIP("SDF 演示文件缺失，跳过");
    }
    auto res = entity::ParserRegistry::defaultRegistry().parseFile(epiSdf(), {});
    REQUIRE(res);
    const auto& scene = *res->scene;
    REQUIRE(scene.instances().size() >= 1u);
    REQUIRE(scene.coordsSoA().positions.size() > 0);
}