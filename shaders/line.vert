// =============================================================================
// MolecuLens · shaders/line.vert —— 线段（线框 / 坐标轴 / 高亮）
// 规格来源：主方案 §5.7.4 / §5.3 LineInst（48B）
// 顶点布局：pos(float4) + color(float4)，GL_LINES 图元
// =============================================================================

#version 450

layout(location = 0) in vec4 aPos;       // xyz = 端点(Å)
layout(location = 1) in vec4 aColor;     // RGBA

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
} ubo;

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = ubo.viewProj * vec4(aPos.xyz, 1.0);
    vColor = aColor;
}
