// =============================================================================
// MolecuLens · tests · vulkan/rt_smoke.cpp —— GPU 光追冒烟（headless）
// 规格来源：任务 E（GPU 光追模块）：headless 设备 + 1 个三角形网格 + 1 个球，
//           64×64 / 1 spp，断言输出非全背景；无 RT 设备时 SKIP（Catch2 WARN）。
// 说明：同时与 CPU 路径对比"非背景像素数"（验收要求 ±10%），并把同一场景
//       渲染两次断言逐字节一致（覆盖管线/SBT/资源复用与生命周期）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/raytrace/ray_tracer.h"
#include "engine/vulkan/device/device.h"
#include "engine/vulkan/raytracing/gpu_ray_tracer.h"

using namespace moleculens;
using namespace moleculens::engine;

namespace {

// 非背景像素计数：以左上角像素为背景参考（几何恒居中，四角必为背景）
uint32_t countNonBackground(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    if (px.size() < 4) {
        return 0;
    }
    const uint8_t r0 = px[0], g0 = px[1], b0 = px[2];
    uint32_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const int dr = std::abs(static_cast<int>(px[i]) - r0);
        const int dg = std::abs(static_cast<int>(px[i + 1]) - g0);
        const int db = std::abs(static_cast<int>(px[i + 2]) - b0);
        if (dr > 24 || dg > 24 || db > 24) {
            ++n;
        }
    }
    (void)w;
    (void)h;
    return n;
}

} // namespace

TEST_CASE("GPU ray tracing renders a triangle and a sphere (CPU parity)",
          "[vulkan][rt][smoke]") {
    constexpr uint32_t kW = 64, kH = 64;

    // 1) headless 设备 + RT 可用性（无 RT 时 SKIP）
    RenderConfig cfg;
    cfg.debug = true;      // 有校验层时启用（device 会自行降级），便于暴露资源错误
    cfg.msaaSamples = 0;
    VulkanDevice dev;
    std::string err;
    if (!dev.initHeadless(cfg, &err)) {
        WARN("Skipping GPU RT smoke (no Vulkan device): " << err);
        return;
    }
    if (!dev.rayTracingEnabled()) {
        WARN("Skipping GPU RT smoke (device has no hardware ray tracing)");
        return;
    }

    GpuRayTracer rt;
    REQUIRE(rt.init(&dev, &err));
    REQUIRE(rt.available());

    // 2) 场景：1 个球 + 1 个三角形（与 CPU 路径同输入）
    RtScene scene;
    RtSphere sphere;
    sphere.center = glm::vec3(0.0f);
    sphere.radius = 2.0f;
    sphere.color = glm::vec3(1.0f, 0.2f, 0.2f);
    scene.spheres.push_back(sphere);

    RtTriangle tri;
    tri.v0 = glm::vec3(-6.0f, -3.0f, 0.0f);
    tri.v1 = glm::vec3(6.0f, -3.0f, 0.0f);
    tri.v2 = glm::vec3(0.0f, 6.0f, 0.0f);
    tri.n0 = tri.n1 = tri.n2 = glm::vec3(0.0f, 0.0f, 1.0f);
    tri.color = glm::vec3(0.2f, 0.4f, 1.0f);
    scene.triangles.push_back(tri);

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 18.0f);
    cam.target = glm::vec3(0.0f);
    cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
    cam.fovDeg = 45.0f;
    cam.width = kW;
    cam.height = kH;

    RtLight light;   // 默认半球光 + 方向光（与 CLI/CPU 路径一致）
    const glm::vec3 bg(0.05f, 0.05f, 0.08f);

    std::vector<uint8_t> gpuPx;
    REQUIRE(rt.render(scene, cam, light, bg, 1, gpuPx));
    REQUIRE(gpuPx.size() == static_cast<size_t>(kW) * kH * 4);
    REQUIRE(rt.lastStats().triangles == 1);
    REQUIRE(rt.lastStats().primitives == 1);

    // 3) 非全背景 + 两个几何都可见（红球 / 蓝三角）
    const uint32_t nonBg = countNonBackground(gpuPx, kW, kH);
    REQUIRE(nonBg > 200);
    uint32_t reddish = 0, bluish = 0;
    for (size_t i = 0; i + 3 < gpuPx.size(); i += 4) {
        if (gpuPx[i] > gpuPx[i + 2] + 30 && gpuPx[i] > 60) {
            ++reddish;
        }
        if (gpuPx[i + 2] > gpuPx[i] + 30 && gpuPx[i + 2] > 60) {
            ++bluish;
        }
    }
    REQUIRE(reddish > 20);
    REQUIRE(bluish > 20);

    // 4) 与 CPU 路径对比非背景像素占比（±10%）
    std::vector<uint8_t> cpuPx;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, cpuPx));
    REQUIRE(cpuPx.size() == gpuPx.size());
    const uint32_t cpuNonBg = countNonBackground(cpuPx, kW, kH);
    REQUIRE(cpuNonBg > 200);
    const double diff =
        std::abs(static_cast<double>(nonBg) - cpuNonBg) / static_cast<double>(cpuNonBg);
    INFO("GPU non-bg = " << nonBg << ", CPU non-bg = " << cpuNonBg
                         << ", rel diff = " << diff);
    REQUIRE(diff < 0.10);

    // 5) 相同输入重复渲染必须逐字节一致（资源复用/生命周期没有副作用）
    std::vector<uint8_t> again;
    REQUIRE(rt.render(scene, cam, light, bg, 1, again));
    REQUIRE(again == gpuPx);
}
