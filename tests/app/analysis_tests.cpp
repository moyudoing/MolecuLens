// =============================================================================
// MolecuLens · tests · app/analysis_tests.cpp —— 逐分子统计 + XLSX 导出
//
// 覆盖：
//   · computeMoleculeStats 的几何/RMSE/RMSF/最大位移口径（手算可复核的直线运动）；
//   · 无轨迹分子的 0 值分支；
//   · writeSheet 生成的 .xlsx 是合法 ZIP（PK 头 + 必要部件名），可由 Excel 打开。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app/workbench/analysis/molecule_stats.h"
#include "app/workbench/analysis/xlsx_writer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::app;
using Catch::Approx;

namespace {

// 2 原子分子：每帧两个原子整体沿 +X 移动 0.1 Å（第 f 帧位移 0.1f）。
// 这个运动是手算 RMSE/RMSF/最大位移的基准。
uint32_t addMovingDiatomic(entity::SceneStructure& scene, uint64_t frames) {
    entity::MoleculeStructure ms;
    for (int i = 0; i < 2; ++i) {
        entity::Atom a{};
        a.element = entity::ElementType::Carbon;
        ms.atoms.push_back(a);
        ms.elements.push_back(entity::ElementType::Carbon);
        ms.atomNameIds.push_back(scene.strings().intern(i == 0 ? "C1" : "C2"));
    }
    ms.key = entity::computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<entity::Trajectory>();
    traj->dt = 1.0f;
    for (uint64_t f = 0; f < frames; ++f) {
        entity::Timestep ts;
        const float dx = 0.1f * static_cast<float>(f);
        ts.positions.emplace_back(dx, 0.0f, 0.0f);
        ts.positions.emplace_back(dx + 1.0f, 0.0f, 0.0f);
        traj->frames.push_back(std::move(ts));
    }
    const uint32_t sys = scene.addSystem("traj");
    return scene.addTrajectoryInstance(si, entity::Transform{}, traj, sys,
                                       "moving", false);
}

} // namespace

TEST_CASE("molecule stats compute geometry, RMSE and RMSF",
          "[app][analysis]") {
    entity::SceneStructure scene;
    addMovingDiatomic(scene, 11);
    scene.rebuildCoordsSoA();

    const MoleculeStats stats = computeMoleculeStats(scene);
    REQUIRE(stats.molecules.size() == 1u);
    const MoleculeStat& m = stats.molecules[0];
    REQUIRE(m.frames == 11u);
    REQUIRE(m.framesUsed == 11u);   // 11 帧 < 200 抽样上限，全部参与
    REQUIRE(m.atoms == 2u);
    REQUIRE(m.bonds == 0u);
    REQUIRE(m.mass == Approx(24.022).margin(1e-3));
    REQUIRE(m.centroid.x == Approx(0.5f).margin(1e-5));
    REQUIRE(m.centroid.y == Approx(0.0f).margin(1e-5));
    REQUIRE(m.extent.x == Approx(1.0f).margin(1e-5));
    REQUIRE(m.radiusOfGyration == Approx(0.5f).margin(1e-5));
    // mean(f²) = 35, 位移 0.1f → MSE = 0.01*35
    REQUIRE(m.rmse == Approx(std::sqrt(0.35f)).margin(1e-4));
    // 位置均匀分布在 0..1（步长 0.1）：方差 = 0.1
    REQUIRE(m.rmsf == Approx(std::sqrt(0.1f)).margin(1e-4));
    REQUIRE(m.maxDisplacement == Approx(1.0f).margin(1e-5));

    const MoleculeStatsSummary& s = stats.summary;
    REQUIRE(s.molecules == 1u);
    REQUIRE(s.atoms == 2u);
    REQUIRE(s.withTrajectory == 1u);
    REQUIRE(s.maxFrames == 11u);
    REQUIRE(s.meanRmse == Approx(m.rmse).margin(1e-6));
    REQUIRE(s.maxRmseName == "moving");
}

TEST_CASE("molecule stats zero trajectory yields zero fluctuation",
          "[app][analysis]") {
    entity::SceneStructure scene;
    entity::MoleculeStructure ms;
    for (int i = 0; i < 3; ++i) {
        entity::Atom a{};
        a.element = entity::ElementType::Carbon;
        ms.atoms.push_back(a);
        ms.elements.push_back(entity::ElementType::Carbon);
        ms.atomNameIds.push_back(scene.strings().intern("C"));
    }
    ms.key = entity::computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    const uint32_t sys = scene.addSystem("static");
    scene.addInstance(si, entity::Transform{}, sys);
    scene.rebuildCoordsSoA();

    const MoleculeStats stats = computeMoleculeStats(scene);
    REQUIRE(stats.molecules.size() == 1u);
    const MoleculeStat& m = stats.molecules[0];
    // 静态结构也带一条单帧轨迹（解析器的统一承载）；没有多帧就没有涨落。
    REQUIRE(m.frames == 1u);
    REQUIRE(m.framesUsed == 1u);
    REQUIRE(m.rmse == 0.0f);
    REQUIRE(m.rmsf == 0.0f);
    REQUIRE(m.maxDisplacement == 0.0f);
    REQUIRE(stats.summary.withTrajectory == 0u);
}

TEST_CASE("molecule stats unwrap periodic boundary jumps",
          "[app][analysis][pbc]") {
    entity::SceneStructure scene;
    entity::MoleculeStructure ms;
    entity::Atom a{};
    a.element = entity::ElementType::Carbon;
    ms.atoms.push_back(a);
    ms.elements.push_back(entity::ElementType::Carbon);
    ms.atomNameIds.push_back(scene.strings().intern("C"));
    ms.key = entity::computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<entity::Trajectory>();
    for (float x : {9.9f, 0.1f}) {   // 跨过 10 Å 正交盒边界，真实位移 0.2 Å
        entity::Timestep ts;
        ts.positions.emplace_back(x, 0.0f, 0.0f);
        ts.box = {10.0f, 0.0f, 0.0f,
                  0.0f, 10.0f, 0.0f,
                  0.0f, 0.0f, 10.0f};
        traj->frames.push_back(std::move(ts));
    }
    const uint32_t sys = scene.addSystem("pbc");
    scene.addTrajectoryInstance(si, entity::Transform{}, traj, sys, "pbc", false);
    scene.rebuildCoordsSoA();

    const MoleculeStats stats = computeMoleculeStats(scene);
    REQUIRE(stats.molecules.size() == 1u);
    const MoleculeStat& m = stats.molecules[0];
    // 未做最小镜像时这里会是 ~9.8 Å（盒长跳变），正确值是 0.2 Å。
    REQUIRE(m.maxDisplacement == Approx(0.2f).margin(1e-4));
    REQUIRE(m.rmse == Approx(std::sqrt(0.04f / 2.0f)).margin(1e-4));
}

TEST_CASE("xlsx writer produces a valid zip workbook", "[app][analysis][xlsx]") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "moleculens_analysis_test.xlsx";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::vector<std::string> headers = {"name", "atoms", "rmse"};
    std::vector<std::vector<xlsx::Cell>> rows = {
        {xlsx::Cell::textCell("分子 A"), xlsx::Cell::numberCell(2),
         xlsx::Cell::numberCell(0.25)},
        {xlsx::Cell::textCell("Molecule B"), xlsx::Cell::numberCell(3),
         xlsx::Cell::numberCell(0.5)},
    };
    std::string err;
    REQUIRE(xlsx::writeSheet(path.string(), "stats", headers, rows, &err));

    std::ifstream f(path, std::ios::binary);
    REQUIRE(f);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 100u);
    REQUIRE(bytes.substr(0, 4) == std::string("PK\x03\x04", 4));
    REQUIRE(bytes.find("xl/worksheets/sheet1.xml") != std::string::npos);
    REQUIRE(bytes.find("xl/workbook.xml") != std::string::npos);
    REQUIRE(bytes.find("xl/styles.xml") != std::string::npos);
    REQUIRE(bytes.find("Molecule B") != std::string::npos);

    std::filesystem::remove(path, ec);
}
