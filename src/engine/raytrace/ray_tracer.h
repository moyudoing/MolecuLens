// =============================================================================
// MolecuLens · engine · raytrace/ray_tracer.h —— 自研 CPU 光线追踪器
// 规格来源：主方案 §6.6（光照模型）；一.7 自研光线追踪集成到分子渲染体系
// 实现：球 + 圆柱（胶囊，圆柱面+球形帽）主光线求交 + 最近命中 + Blinn-Phong +
//       软阴影（光源抖动采样）+ 球体 1 次反射；按行多线程并行。
// 用途：图像导出（RT 渲染）与 3D 视口 RT 预览。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "engine/cmd/cmd.h"                 // SceneCommandList（buildRtScene 输入）
#include "engine/render/render_parameter.h"  // LightParams（光照对齐）

namespace moleculens::engine {

// —— 场景图元（直接来自命令流 SphereInst/CylinderInst）——
// color = 材质 diffuse × 图元色（已烘焙）。其余材质字段与 GPU 侧 MaterialBlock
// 同义（P6 起参与着色：环境/高光/光泽度/粗糙度/金属度），默认值 = "默认"预设。
struct RtSphere {
    glm::vec3 center;
    float radius;
    glm::vec3 color;
    glm::vec3 ambient{0.60f};      // matAmbient
    glm::vec3 specular{0.30f};     // matSpecular
    float shininess = 32.0f;
    float roughness = 0.40f;
    float metallic = 0.0f;
    // 不透明度（MaterialBlock::opacity）。<1 时 RT 会继续追踪表面后方并与
    // 命中色做 alpha 混合 —— GPU 路径一直是半透明的，RT 此前完全忽略该字段。
    float opacity = 1.0f;
};
struct RtCylinder {
    glm::vec3 a, b;        // 胶囊两端（圆柱面 + 两端球形帽）
    float radius;
    glm::vec3 color;
    glm::vec3 ambient{0.60f};
    glm::vec3 specular{0.30f};
    float shininess = 32.0f;
    float roughness = 0.40f;
    float metallic = 0.0f;
    float opacity = 1.0f;
};
// 三角网格（P1：表面 / QuickSurf / 卡通等 TriMesh 命令）
// 顶点法线用于平滑着色（重心插值）；颜色已烘焙（顶点色 × 材质 diffuse）。
struct RtTriangle {
    glm::vec3 v0{0.0f}, v1{0.0f}, v2{0.0f};
    glm::vec3 n0{0.0f, 1.0f, 0.0f}, n1{0.0f, 1.0f, 0.0f}, n2{0.0f, 1.0f, 0.0f};
    glm::vec3 color{0.8f};
    glm::vec3 ambient{0.60f};
    glm::vec3 specular{0.30f};
    float shininess = 32.0f;
    float roughness = 0.40f;
    float metallic = 0.0f;
    float opacity = 1.0f;
};
struct RtScene {
    std::vector<RtSphere> spheres;
    std::vector<RtCylinder> cylinders;
    std::vector<RtTriangle> triangles;   // 三角网格（表面类表示）
};

// 相机（位置/目标/上向量/视场/分辨率）
struct RtCamera {
    glm::vec3 position{0.0f, 0.0f, 40.0f};
    glm::vec3 target{0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float fovDeg = 45.0f;
    uint32_t width = 640, height = 480;
};

// 光照（与 LightParams 语义一致：半球环境光 + 方向光）。
// P6 起与 GPU 侧对齐：方向光参与 Lambert 漫反射 + Blinn-Phong 高光，并可投射软阴影。
struct RtLight {
    glm::vec3 skyColor{1.0f, 1.0f, 1.0f};          // 天顶色（半球上方）
    glm::vec3 groundColor{0.35f, 0.4f, 0.5f};      // 地面色（半球下方）
    glm::vec3 ambientColor{0.55f, 0.6f, 0.75f};    // 均匀环境光基础色
    float ambientIntensity = 0.45f;                // 半球光强度
    // —— 方向光（与 GPU 侧 LightParams 同义）——
    glm::vec3 dirLightDir{0.40f, 0.80f, 0.60f};    // 指向光源（着色器内归一化）
    glm::vec3 dirLightColor{1.0f, 0.98f, 0.95f};
    float dirLightIntensity = 0.35f;
    bool  dirLightEnabled = true;
    float ambientWeight = 1.0f;
    float diffuseWeight = 1.0f;
    float specularWeight = 1.0f;
    // 方向光软阴影采样数：0/1 = 硬阴影（单条阴影光线），>1 = 光源方向锥内确定性
    // 抖动采样求平均（P6 起真正启用；此前仅占位字段，从不参与计算）。
    int shadowSamples = 0;
    int maxBounces = 1;         // 反射深度（球体）
};

// —— RT 级 LOD：可复用的加速结构 ——
// 交互式 RT 预览里相机每动一次就重建 BVH 太贵（91 万三角形实测 ~119 ms），
// 所以把建树结果缓存下来：场景（命令流）不变时只重做光线投射。
struct RtAccel;

// 为场景构建加速结构；场景为空时返回 nullptr（调用方回退到 rayTrace 内部建树）。
std::shared_ptr<RtAccel> buildRtAccel(const RtScene& scene);

// 光线追踪渲染（RGBA8，行序 top→bottom；多线程并行）。
// samplesPerPixel = 每像素子采样数（1 = 最快；≥2 抗锯齿）。
// backgroundColor = 未命中背景色。
// accel = 可选的预建加速结构（nullptr 时按场景临时建树）。
bool rayTrace(const RtScene& scene, const RtCamera& cam, const RtLight& light,
              const glm::vec3& backgroundColor, int samplesPerPixel,
              std::vector<uint8_t>& outRgba,
              const std::shared_ptr<RtAccel>& accel = nullptr);

// 从命令流构建 RT 场景（把当前材质**完整**烘焙进图元，与光栅化视觉一致）
bool buildRtScene(const SceneCommandList& list, RtScene& out);

// 从光栅化侧 LightParams 填装 RtLight —— 两条渲染路径共用同一光照来源，
// 避免"改一处忘一处"（此前调用方各自手抄 4 行映射）。
void rtLightFrom(const LightParams& src, RtLight& out, int shadowSamples, int maxBounces);

} // namespace moleculens::engine
