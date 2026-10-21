// =============================================================================
// MolecuLens · shaders/point.vert —— 点模式（体系渲染模式 = 点）
// 输入 = SphereInst 实例数据（位置/半径→点大小/颜色），输出 GL_POINTS。
// 实例属性布局对应 C++ SphereInst（48B，与 sphere.vert 一致）。
// UBO 布局来自 global_ubo.glsl（唯一定义处）
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

// 实例数据（binding 1）：SphereInst 48B（仅实例属性；无 location 0 顶点输入）
layout(location = 1) in vec4  instPosRadius;   // xyz 中心, w 半径
layout(location = 2) in vec4  instColor;       // RGBA
layout(location = 3) in uvec2 instPickId;      // 拾取 ID
layout(location = 4) in uvec2 instPad;         // 0

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vDist;          // 到相机距离（雾）
layout(location = 2) out vec3 vPos;            // 世界坐标（剖切面）

void main() {
    const vec3 worldPos = instPosRadius.xyz * ubo.sceneProps.x;
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    // 点大小按**透视距离**换算（此前固定 24×半径：远近距离一样大，完全错误）。
    // 屏幕高度 = 2·tan(fov/2)·dist 对应 sceneProps.y 像素，故
    //   直径(px) = 2·radius·sceneScale / (2·tanHalf·dist) · viewportH
    // sceneProps.y = 视口高度，sceneProps.z = 2·tan(fov/2)。
    const float dist = max(length(worldPos - ubo.cameraPos.xyz), 0.05);
    const float tanHalf = max(ubo.sceneProps.z * 0.5, 0.05);
    const float pointPx =
        2.0 * instPosRadius.w * ubo.sceneProps.x * ubo.sceneProps.y /
        (2.0 * tanHalf * dist);
    // 近处别糊满屏，远处至少 1.5 px 可见
    gl_PointSize = clamp(pointPx, 1.5, 64.0);
    vColor = instColor;
    vDist = dist;
    vPos = worldPos;
}
