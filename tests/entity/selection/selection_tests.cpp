// =============================================================================
// MolecuLens · tests · entity/selection/selection_tests.cpp —— 选择语言测试
// 规格来源：主方案 §4.6 / §14.3（语法解析/求值）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "entity/selection/selection.h"

using namespace moleculens::entity;

namespace {

// 构造 4 原子测试场景：C1(链A,残基1) C2(链A,残基1) N3(链A,残基2) O4(链B,残基3)
struct TestScene {
    SceneStructure scene;
    std::vector<glm::vec3> positions;

    TestScene() {
        positions = {
            {0, 0, 0},   // C1
            {1.5f, 0, 0}, // C2
            {3.0f, 0, 0}, // N3
            {0, 3.0f, 0}, // O4
        };
        MoleculeStructure ms;
        ms.elements = {ElementType::Carbon, ElementType::Carbon, ElementType::Nitrogen,
                       ElementType::Oxygen};
        ms.atoms.resize(4);
        ms.atoms[0] = {ElementType::Carbon, 100, 1000, 0, 0};
        ms.atoms[1] = {ElementType::Carbon, 100, 2000, 0, 0};
        ms.atoms[2] = {ElementType::Nitrogen, 100, 3000, 1, 0};
        ms.atoms[3] = {ElementType::Oxygen, 50, 4000, 2, 1};
        ms.atomNameIds = {0, 0, 0, 0};
        // 名字驻留
        ms.atomNameIds[0] = scene.strings().intern("C1");
        ms.atomNameIds[1] = scene.strings().intern("C2");
        ms.atomNameIds[2] = scene.strings().intern("N3");
        ms.atomNameIds[3] = scene.strings().intern("O4");
        ms.bonds = {{0, 1, 1}, {1, 2, 1}};
        ms.key = computeStructureKey(ms);
        const uint32_t si = scene.registerStructure(ms);
        // 链/残基表
        scene.addChain("A");
        scene.addChain("B");
        scene.addResidue("R1");
        scene.addResidue("R2");
        scene.addResidue("R3");
        // 实例 + 帧
        auto traj = std::make_shared<Trajectory>();
        Timestep ts;
        ts.positions = positions;
        traj->frames.push_back(std::move(ts));
        scene.addTrajectoryInstance(si, Transform{}, std::move(traj));
        scene.rebuildCoordsSoA();
    }

    FrameData frame() const {
        FrameData fd;
        fd.positions = &scene.coordsSoA().positions;
        return fd;
    }
};

// 全局测试夹具（单例）
static TestScene TEST_SCENE;

} // namespace

TEST_CASE("Selection parse valid expressions", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("all", table));
    REQUIRE(sel.parse("element C", table));
    REQUIRE(sel.parse("element C AND element N", table));
    REQUIRE(sel.parse("element C OR element N", table));
    REQUIRE(sel.parse("NOT element C", table));
    REQUIRE(sel.parse("(element C OR element N) AND NOT element O", table));
    REQUIRE(sel.parse("name C1", table));
    REQUIRE(sel.parse("chain A", table));
    REQUIRE(sel.parse("resid 1-2", table));
    REQUIRE(sel.parse("occupancy > 0.5", table));
    REQUIRE(sel.parse("within 5.0 of element O", table));
}

TEST_CASE("Selection parse rejects invalid expressions", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE_FALSE(sel.parse("element", table));          // 缺值
    REQUIRE_FALSE(sel.parse("(", table));                // 括号不闭合
    REQUIRE_FALSE(sel.parse("element C AND", table));    // 尾随运算符
    REQUIRE_FALSE(sel.parse("foo bar", table));          // 未知关键字
}

TEST_CASE("Selection evaluate element mask", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("element C", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 2);
    const auto& mask = sel.mask();
    REQUIRE(mask.size() == 4);
    REQUIRE(mask[0]);
    REQUIRE(mask[1]);
    REQUIRE_FALSE(mask[2]);
    REQUIRE_FALSE(mask[3]);
}

TEST_CASE("Selection evaluate boolean ops", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("element C OR element O", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 3);
    REQUIRE(sel.parse("element C AND element N", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 0);
    REQUIRE(sel.parse("NOT element C", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 2);
}

TEST_CASE("Selection evaluate name/resid/chain", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("name N3", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 1);
    REQUIRE(sel.mask()[2]);

    REQUIRE(sel.parse("chain A", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 3);

    REQUIRE(sel.parse("resid 1-1", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 1);   // residueId==1 仅 N3
}

TEST_CASE("Selection evaluate bfactor/occupancy", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("occupancy > 50", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 3);   // O4 occupancy=50 不满足 >50
    REQUIRE(sel.parse("bfactor > 25", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 2);   // 10/20 < 25，30/40 > 25
}

TEST_CASE("Selection evaluate within distance", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("within 2.0 of element O", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    // O4(0,3,0) 距离 3.0 内无原子（C1 距离 3.0 不满足 <2.0）
    REQUIRE(sel.selected() == 1);   // 仅 O4 自身
}

TEST_CASE("Selection evaluate all/none", "[selection]") {
    const ParseTable& table = ParseTable::defaultTable();
    Selection sel;
    REQUIRE(sel.parse("all", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 4);
    REQUIRE(sel.parse("none", table));
    sel.evaluate(TEST_SCENE.scene, TEST_SCENE.frame(), 0);
    REQUIRE(sel.selected() == 0);
}
