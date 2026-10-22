// =============================================================================
// MolecuLens · tests · engine/camera/camera_tests.cpp —— 相机/投影数学
// 规格来源：主方案 §14.3（相机/投影数学）
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/camera/camera.h"

using namespace moleculens::engine;

TEST_CASE("OrbitCamera position follows target/distance/yaw/pitch", "[camera]") {
    OrbitCamera cam;
    cam.target = {0, 0, 0};
    cam.distance = 10.0f;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    // pitch=0 → 在目标水平面（+Y 朝上）
    const glm::vec3 pos = cam.position();
    REQUIRE(glm::length(pos - glm::vec3(0.0f)) == Catch::Approx(10.0f).margin(1e-4f));
    REQUIRE(pos.y == Catch::Approx(0.0f).margin(1e-4f));
}

TEST_CASE("OrbitCamera centerOn fits bounding sphere", "[camera]") {
    OrbitCamera cam;
    cam.centerOn({1.0f, 2.0f, 3.0f}, 5.0f);
    REQUIRE(cam.target == glm::vec3(1.0f, 2.0f, 3.0f));
    // distance = radius / tan(fov/2) * 1.2（主方案 §5.5）
    const float expect = 5.0f / std::tan(glm::radians(45.0f) * 0.5f) * 1.2f;
    REQUIRE(cam.distance == Catch::Approx(expect).margin(1e-3f));
    // near/far（主方案 §5.5）
    REQUIRE(cam.nearClip == Catch::Approx(5.0f * 1e-3f).margin(1e-6f));
    REQUIRE(cam.farClip == Catch::Approx(50.0f).margin(1e-3f));
}

TEST_CASE("OrbitCamera view-projection is invertible", "[camera]") {
    OrbitCamera cam;
    cam.centerOn({0, 0, 0}, 10.0f);
    const glm::mat4 vp = cam.viewProjMatrix(1.5f);
    REQUIRE(glm::determinant(vp) != 0.0f);
}

TEST_CASE("OrbitCamera orbit clamps pitch", "[camera]") {
    OrbitCamera cam;
    cam.orbit(0.5f, 3.0f);
    REQUIRE(cam.pitch <= 1.55f);
    cam.orbit(0.0f, -5.0f);
    REQUIRE(cam.pitch >= -1.55f);
    REQUIRE(cam.yaw == Catch::Approx(0.5f).margin(1e-4f));
}

TEST_CASE("OrbitCamera dolly clamps distance", "[camera]") {
    OrbitCamera cam;
    cam.dolly(0.0001f);
    REQUIRE(cam.distance >= cam.nearClip * 10.0f);
    cam.dolly(1e6f);
    REQUIRE(cam.distance <= cam.farClip * 0.5f);
}

TEST_CASE("OrbitCamera zoom cannot enter the bounding sphere", "[camera]") {
    OrbitCamera cam;
    cam.centerOn({0.0f, 0.0f, 0.0f}, 50.0f);
    REQUIRE(cam.minDistance == Catch::Approx(7.5f).margin(1e-4f));
    // 无限放大：必须停在 0.15r，否则相机钻进分子、近裁面把球/键切成断面。
    for (int i = 0; i < 64; ++i) {
        cam.dolly(0.5f);
    }
    REQUIRE(cam.distance == Catch::Approx(7.5f).margin(1e-3f));
    // 近裁面随距离收缩：镜头贴近表面时仍远小于原子尺度。
    REQUIRE(cam.nearClip <= cam.distance * 1e-3f + 1e-6f);
    REQUIRE(cam.nearClip >= 1e-4f);
}
