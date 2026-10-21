// =============================================================================
// MolecuLens · shaders/pick_cyl.vert —— 拾取 ID Pass（圆柱实例化）
// 实例布局 = CylinderInst（64B）：a/b/radius/color/pickId
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec3 aLocalPos;

layout(location = 1) in vec4  instA;
layout(location = 2) in vec4  instB;
layout(location = 3) in vec4  instRadius;
layout(location = 4) in vec4  instColor;
layout(location = 5) in uvec2 instPickId;

layout(location = 0) out flat uint vPickId;

void main() {
    // 全局缩放须与 cylinder.vert 一致，否则拾取几何与渲染几何错位
    vec3 a = instA.xyz * ubo.sceneProps.x;
    vec3 b = instB.xyz * ubo.sceneProps.x;
    float r = instRadius.x * ubo.sceneProps.x;
    vec3 axis = b - a;
    float halfLen = length(axis) * 0.5;
    vec3 dir = halfLen > 1e-6 ? axis / (2.0 * halfLen) : vec3(0.0, 1.0, 0.0);
    vec3 up = abs(dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, dir));
    vec3 up2 = cross(dir, right);
    // 与 cylinder.vert 一致：以两端点中点为原点（此前用端点 A 为原点 → 拾取几何
    // 相对渲染几何偏移半个键长，拾取会点不中键——虽然 pick pass 尚未落地）
    vec3 mid = 0.5 * (a + b);
    vec3 worldPos = mid + dir * (aLocalPos.y * halfLen)
                     + right * (aLocalPos.x * r) + up2 * (aLocalPos.z * r);
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vPickId = instPickId.x;
}
