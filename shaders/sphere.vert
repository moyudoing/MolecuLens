// =============================================================================
// MolecuLens · shaders/sphere.vert —— 实例化原子球（空间填充 / 球棍）
// 规格来源：主方案 §5.7.1（单位 icosphere + per-instance 位置/半径/色/拾取 ID）
// 实例属性布局对应 C++ 载荷 SphereInst（48B，主方案 §5.3/附录 F）
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec3 aLocalPos;      // 共享单位 icosphere 顶点

// 实例数据（binding 1）：SphereInst 48B
layout(location = 1) in vec4  instPosRadius;   // xyz 中心, w 半径
layout(location = 2) in vec4  instColor;       // RGBA
layout(location = 3) in uvec2 instPickId;      // 拾取 ID
layout(location = 4) in uvec2 instPad;         // 0

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vPos;
layout(location = 3) out vec4 vPickId;
layout(location = 4) out float vDist;

void main() {
    // 解析法线：片元法线 = normalize(局部坐标)，主方案 §5.7.1
    const vec3 local = aLocalPos;
    // sceneProps.x = 全局缩放：绕世界原点等比缩放（球心与半径同乘，均匀缩放不改法线）
    const vec3 worldPos =
        (instPosRadius.xyz + local * instPosRadius.w) * ubo.sceneProps.x;
    const vec3 worldN = normalize(local);

    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vColor = instColor;
    vNormal = worldN;
    vPos = worldPos;
    vPickId = vec4(vec2(instPickId), 0.0, 0.0);
    vDist = length(worldPos - ubo.cameraPos.xyz);
}
