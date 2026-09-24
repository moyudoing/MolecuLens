// =============================================================================
// MolecuLens · tests · engine/raytrace/ray_tracer_tests.cpp —— CPU 光线追踪器
// 规格来源：一.7 自研光线追踪（单球非背景 / 双球遮挡 / 胶囊命中 / 命令流构建）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "engine/cmd/cmd.h"
#include "engine/raytrace/ray_tracer.h"

using namespace moleculens::engine;

namespace {

// 统计 RGBA 中与背景（纯白）不同的像素数
uint32_t countNonBg(const std::vector<uint8_t>& px, const uint8_t bg[3]) {
    uint32_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] != bg[0] || px[i + 1] != bg[1] || px[i + 2] != bg[2]) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("ray tracer renders a single sphere (non-background pixels)",
          "[engine][raytrace]") {
    RtScene scene;
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, 0.0f), 1.0f, glm::vec3(1.0f, 0.2f, 0.2f)});

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 4.0f);
    cam.target = glm::vec3(0.0f);
    cam.fovDeg = 45.0f;
    cam.width = 64;
    cam.height = 48;

    RtLight light;                 // 默认光源
    light.shadowSamples = 1;       // 硬阴影，快
    light.maxBounces = 0;

    const glm::vec3 bg(1.0f);      // 白背景
    std::vector<uint8_t> px;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, px));
    REQUIRE(px.size() == 64u * 48u * 4u);

    const uint8_t bgc[3] = {255, 255, 255};
    const uint32_t nonBg = countNonBg(px, bgc);
    REQUIRE(nonBg > 100);   // 球投影占据大部分像素

    // 球为红色调：命中像素 R 明显 > B（照明下保持色相）
    uint32_t reddish = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] > px[i + 2] + 30 && px[i] > 60) {
            ++reddish;
        }
    }
    REQUIRE(reddish > 50);
}

TEST_CASE("ray tracer front sphere occludes back sphere", "[engine][raytrace]") {
    // 前面（近相机 z=+1）大蓝球遮挡后面（z=-1）小红球
    RtScene scene;
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, 1.0f), 0.6f, glm::vec3(0.2f, 0.2f, 1.0f)});
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, -1.0f), 0.4f, glm::vec3(1.0f, 0.1f, 0.1f)});

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 3.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 32;
    cam.height = 32;

    RtLight light;
    light.shadowSamples = 1;
    light.maxBounces = 0;

    const glm::vec3 bg(1.0f);
    std::vector<uint8_t> px;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, px));

    // 中心像素应命中蓝球（R 低、B 高）
    const size_t c = (16u * 32u + 16u) * 4u;
    REQUIRE(px[c + 2] > px[c] + 30);   // B > R + 30 → 蓝
}

TEST_CASE("ray tracer renders a capsule (cylinder)", "[engine][raytrace]") {
    RtScene scene;
    scene.cylinders.push_back(
        {glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.4f,
         glm::vec3(0.1f, 0.9f, 0.1f)});   // 横置绿色胶囊

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 3.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 48;
    cam.height = 32;

    RtLight light;
    light.shadowSamples = 1;
    light.maxBounces = 0;

    const glm::vec3 bg(1.0f);
    std::vector<uint8_t> px;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, px));

    const uint8_t bgc[3] = {255, 255, 255};
    const uint32_t nonBg = countNonBg(px, bgc);
    REQUIRE(nonBg > 100);   // 胶囊投影面积可观
}

TEST_CASE("buildRtScene decodes spheres and cylinders with material diffuse",
          "[engine][raytrace]") {
    SceneCommandList list;
    MaterialBlock mat;
    mat.diffuse = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));

    SphereInst s;
    s.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    s.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    list.append(Cmd::SphereInst, &s, sizeof(s));

    CylinderInst c;
    c.aPos = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    c.bPos = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    c.radius = 0.2f;
    c.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    list.append(Cmd::CylinderInst, &c, sizeof(c));

    RtScene scene;
    REQUIRE(buildRtScene(list, scene));
    REQUIRE(scene.spheres.size() == 1u);
    REQUIRE(scene.cylinders.size() == 1u);
    // 材质 diffuse 0.5 烘焙进颜色
    REQUIRE(scene.spheres[0].color == glm::vec3(0.5f));
    REQUIRE(scene.cylinders[0].color == glm::vec3(0.5f));
    REQUIRE(std::fabs(scene.cylinders[0].radius - 0.2f) < 1e-4f);
}
