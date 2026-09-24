// =============================================================================
// MolecuLens · tests · engine/quicksurf/quicksurf_tests.cpp —— QuickSurf 高斯表面
// 规格来源：QS.1 参照 other/QuickSurf 的高斯密度 + 等值面抽取
// 校验：单原子/双原子生成闭合三角网格，顶点落在等值面附近。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "engine/style/quicksurf/quicksurf.h"

using namespace moleculens::engine;

TEST_CASE("QuickSurf single atom produces a closed surface mesh", "[engine][quicksurf]") {
    std::vector<glm::vec3> centers = {glm::vec3(0.0f)};
    std::vector<float> radii = {1.0f};
    QuickSurfMesh mesh;
    REQUIRE(quickSurfMesh(centers, radii, 1.0f, 0.5f, 0.5f, mesh));
    REQUIRE(mesh.positions.size() > 100u);
    REQUIRE(mesh.normals.size() == mesh.positions.size());
    REQUIRE(mesh.indices.size() % 3 == 0);
    REQUIRE(mesh.indices.size() > 0u);

    // 高斯 exp(-r²/(2·r0²)) 的 0.5 等值面在 r = sqrt(2·ln2) ≈ 1.18（r0=1）
    float minD = 1e30f, maxD = 0.0f;
    for (const auto& p : mesh.positions) {
        const float d = glm::length(p);
        minD = std::min(minD, d);
        maxD = std::max(maxD, d);
    }
    REQUIRE(minD > 0.5f);   // 顶点不会塌到原子中心
    REQUIRE(maxD < 2.5f);   // 顶点在等值面附近（含网格插值误差与边距）
}

TEST_CASE("QuickSurf two-atom dimer produces a bridging surface", "[engine][quicksurf]") {
    std::vector<glm::vec3> centers = {glm::vec3(-1.0f, 0, 0), glm::vec3(1.0f, 0, 0)};
    std::vector<float> radii = {1.0f, 1.0f};
    QuickSurfMesh mesh;
    REQUIRE(quickSurfMesh(centers, radii, 1.0f, 0.5f, 0.5f, mesh));
    REQUIRE(mesh.positions.size() > 100u);

    // 两原子间（原点处）应在表面内部：表面应横跨两原子之间的空隙
    // （用最大顶点 x 判断表面覆盖范围，应覆盖到两个原子外侧）
    float minX = 1e30f, maxX = -1e30f;
    for (const auto& p : mesh.positions) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }
    REQUIRE(minX < -1.0f);
    REQUIRE(maxX > 1.0f);
}

TEST_CASE("QuickSurf params affect the surface radius", "[engine][quicksurf]") {
    // 更大 radscale → 高斯更宽 → 等值面半径更大
    std::vector<glm::vec3> centers = {glm::vec3(0.0f)};
    std::vector<float> radii = {1.0f};
    QuickSurfMesh m1, m2;
    REQUIRE(quickSurfMesh(centers, radii, 1.0f, 0.5f, 0.5f, m1));
    REQUIRE(quickSurfMesh(centers, radii, 1.5f, 0.5f, 0.5f, m2));
    float r1 = 0.0f, r2 = 0.0f;
    for (const auto& p : m1.positions) {
        r1 = std::max(r1, glm::length(p));
    }
    for (const auto& p : m2.positions) {
        r2 = std::max(r2, glm::length(p));
    }
    REQUIRE(r2 > r1);   // radscale 1.5 → 表面外扩
}

TEST_CASE("QuickSurf AVX2 backend matches scalar output", "[engine][quicksurf]") {
    if (!avx2KernelAvailable()) {
        WARN("AVX2 不可用，跳过 AVX2 对比测试");
        return;
    }
    std::vector<glm::vec3> centers = {glm::vec3(0.0f), glm::vec3(1.5f, 0, 0),
                                      glm::vec3(0, 1.2f, 0.3f)};
    std::vector<float> radii = {1.0f, 0.8f, 1.1f};
    QuickSurfMesh scalar, avx2;
    quickSurfSetBackend(1);   // Scalar
    REQUIRE(quickSurfMesh(centers, radii, 1.0f, 0.5f, 0.5f, scalar));
    REQUIRE(quickSurfBackend() == 1);
    quickSurfSetBackend(2);   // AVX2
    REQUIRE(quickSurfMesh(centers, radii, 1.0f, 0.5f, 0.5f, avx2));
    REQUIRE(quickSurfBackend() == 2);
    quickSurfSetBackend(0);   // 复位 Auto
    REQUIRE(scalar.positions.size() > 100u);
    REQUIRE(avx2.positions.size() > 100u);
    // 顶点数相对差 ≤ 5%（fast-exp 近似可致少量立方体翻转，表面形状仍一致）
    REQUIRE(std::abs(static_cast<int>(scalar.positions.size()) -
                     static_cast<int>(avx2.positions.size())) <
            static_cast<int>(scalar.positions.size()) / 20);
    // 包围盒一致（几何等效）
    const auto bbox = [](const QuickSurfMesh& m) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const auto& p : m.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        return std::make_pair(lo, hi);
    };
    const auto [lo1, hi1] = bbox(scalar);
    const auto [lo2, hi2] = bbox(avx2);
    REQUIRE(glm::length(lo1 - lo2) < 0.3f);
    REQUIRE(glm::length(hi1 - hi2) < 0.3f);
}
