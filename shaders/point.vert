// =============================================================================
// MolecuLens · shaders/point.vert —— 点模式（体系渲染模式 = 点）
// 输入 = SphereInst 实例数据（位置/半径→点大小/颜色），输出 GL_POINTS。
// 实例属性布局对应 C++ SphereInst（48B，与 sphere.vert 一致）。
// =============================================================================

#version 450

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
} ubo;

// 实例数据（binding 1）：SphereInst 48B（仅实例属性；无 location 0 顶点输入）
layout(location = 1) in vec4  instPosRadius;   // xyz 中心, w 半径
layout(location = 2) in vec4  instColor;       // RGBA
layout(location = 3) in uvec2 instPickId;      // 拾取 ID
layout(location = 4) in uvec2 instPad;         // 0

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = ubo.viewProj * vec4(instPosRadius.xyz, 1.0);
    // 点大小随半径（世界→屏幕近似；clamp 到 [2, 24]，需 largePoints 特性）
    gl_PointSize = clamp(instPosRadius.w * 24.0, 2.0, 24.0);
    vColor = instColor;
}
