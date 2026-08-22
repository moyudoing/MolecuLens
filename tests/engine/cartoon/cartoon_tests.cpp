// =============================================================================
// MolecuLens · tests · engine/cartoon/cartoon_tests.cpp —— 卡通 SS 判定
// 规格来源：主方案 §14.4.1（几何启发正确性/阈值边界/断链重置/缺失原子）
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/style/cartoon/cartoon.h"

using namespace moleculens::engine;

TEST_CASE("calcPhiPsi computes dihedral angles", "[cartoon]") {
    // 标准构型：四点共面 +Z 侧
    const glm::vec3 prevN(0, 0, 0), ca(1, 0, 0), c(1.5f, 1.0f, 0), nextN(2.5f, 1.0f, 0);
    float phi = 0, psi = 0;
    REQUIRE(calcPhiPsi(prevN, ca, c, nextN, &phi, &psi));
    // 共面 → 二面角 0° 或 180°（取决于方向）
    const bool coplanar =
        std::fabs(phi) < 1.0f || std::fabs(std::fabs(phi) - 180.0f) < 1.0f;
    REQUIRE(coplanar);
}

TEST_CASE("CartoonConstants defaults match spec", "[cartoon]") {
    const CartoonConstants K;
    REQUIRE(K.breakThreshold == Catch::Approx(4.2f));
    REQUIRE(K.samplesPerSegment == 4);
    REQUIRE(K.tubeSegments == 8);
    REQUIRE(K.coilRadius == Catch::Approx(0.35f));
    REQUIRE(K.helixWidth == Catch::Approx(1.6f));
    REQUIRE(K.sheetStartWidth == Catch::Approx(1.2f));
    REQUIRE(K.sheetEndWidth == Catch::Approx(2.4f));
    // SS 色（主方案 §5.7.9）
    REQUIRE(K.helixColor[0] == Catch::Approx(1.00f));
    REQUIRE(K.helixColor[1] == Catch::Approx(0.25f));
    REQUIRE(K.helixColor[2] == Catch::Approx(0.25f));
    REQUIRE(K.sheetColor[0] == Catch::Approx(1.00f));
    REQUIRE(K.sheetColor[1] == Catch::Approx(0.85f));
    REQUIRE(K.sheetColor[2] == Catch::Approx(0.20f));
    REQUIRE(K.coilColor[0] == Catch::Approx(0.35f));
    REQUIRE(K.coilColor[1] == Catch::Approx(0.85f));
    REQUIRE(K.coilColor[2] == Catch::Approx(0.40f));
}
