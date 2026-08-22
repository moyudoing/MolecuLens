// =============================================================================
// MolecuLens · shaders/pick.vert —— 拾取 ID Pass（球实例化）
// 规格来源：主方案 §5.6（离屏 ID Pass，R32_UINT，无光照）
// 实例布局 = SphereInst（48B）：posRadius/color/pickId/pad
// =============================================================================

#version 450

layout(location = 0) in vec3 aLocalPos;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
} ubo;

layout(location = 1) in vec4  instPosRadius;
layout(location = 2) in vec4  instColor;
layout(location = 3) in uvec2 instPickId;
layout(location = 4) in uvec2 instPad;

layout(location = 0) out flat uint vPickId;

void main() {
    vec3 worldPos = instPosRadius.xyz + aLocalPos * instPosRadius.w;
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vPickId = instPickId.x;
}
