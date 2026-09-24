// =============================================================================
// MolecuLens · engine · raytrace/ray_tracer.h —— 自研 CPU 光线追踪器
// 规格来源：主方案 §6.6（光照模型）；一.7 自研光线追踪集成到分子渲染体系
// 实现：球 + 圆柱（胶囊，圆柱面+球形帽）主光线求交 + 最近命中 + Blinn-Phong +
//       软阴影（光源抖动采样）+ 球体 1 次反射；按行多线程并行。
// 用途：图像导出（RT 渲染）与 3D 视口 RT 预览。
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "engine/cmd/cmd.h"   // SceneCommandList（buildRtScene 输入）

namespace moleculens::engine {

// —— 场景图元（直接来自命令流 SphereInst/CylinderInst）——
struct RtSphere {
    glm::vec3 center;
    float radius;
    glm::vec3 color;
};
struct RtCylinder {
    glm::vec3 a, b;        // 胶囊两端（圆柱面 + 两端球形帽）
    float radius;
    glm::vec3 color;
};
struct RtScene {
    std::vector<RtSphere> spheres;
    std::vector<RtCylinder> cylinders;
};

// 相机（位置/目标/上向量/视场/分辨率）
struct RtCamera {
    glm::vec3 position{0.0f, 0.0f, 40.0f};
    glm::vec3 target{0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float fovDeg = 45.0f;
    uint32_t width = 640, height = 480;
};

// 光照（与 LightParams/全局光语义一致：半球环境光，无方向光/点光源）
struct RtLight {
    glm::vec3 skyColor{1.0f, 1.0f, 1.0f};          // 天顶色（半球上方）
    glm::vec3 groundColor{0.35f, 0.4f, 0.5f};      // 地面色（半球下方）
    glm::vec3 ambientColor{0.55f, 0.6f, 0.75f};    // 均匀环境光基础色
    float ambientIntensity = 0.45f;                // 半球光强度
    int shadowSamples = 0;      // 保留字段（全局光无方向阴影；0 = 不使用）
    int maxBounces = 1;         // 反射深度（球体）
};

// 光线追踪渲染（RGBA8，行序 top→bottom；多线程并行）。
// samplesPerPixel = 每像素子采样数（1 = 最快；≥2 抗锯齿）。
// backgroundColor = 未命中背景色。
bool rayTrace(const RtScene& scene, const RtCamera& cam, const RtLight& light,
              const glm::vec3& backgroundColor, int samplesPerPixel,
              std::vector<uint8_t>& outRgba);

// 从命令流构建 RT 场景（把当前材质 diffuse 烘焙进图元色，与光栅化视觉一致）
bool buildRtScene(const SceneCommandList& list, RtScene& out);

} // namespace moleculens::engine
