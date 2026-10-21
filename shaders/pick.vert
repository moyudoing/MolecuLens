// =============================================================================
// MolecuLens · shaders/pick.vert —— 拾取 ID Pass（球实例化）
// 规格来源：主方案 §5.6（离屏 ID Pass，R32_UINT，无光照）
// 实例布局 = SphereInst（48B）：posRadius/color/pickId/pad
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec3 aLocalPos;

layout(location = 1) in vec4  instPosRadius;
layout(location = 2) in vec4  instColor;
layout(location = 3) in uvec2 instPickId;
layout(location = 4) in uvec2 instPad;

layout(location = 0) out flat uint vPickId;

void main() {
    // 全局缩放须与 sphere.vert 一致，否则拾取几何与渲染几何错位
    const vec3 worldPos =
        (instPosRadius.xyz + aLocalPos * instPosRadius.w) * ubo.sceneProps.x;
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vPickId = instPickId.x;
}
