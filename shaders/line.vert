// =============================================================================
// MolecuLens · shaders/line.vert —— 线段（线框 / 坐标轴 / 高亮）
// 规格来源：主方案 §5.7.4 / §5.3 LineInst（48B）
// 顶点布局：pos(float4) + color(float4)，GL_LINES 图元
// UBO 布局来自 global_ubo.glsl（唯一定义处）
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec4 aPos;       // xyz = 端点(Å)
layout(location = 1) in vec4 aColor;     // RGBA

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vDist;    // 到相机距离（雾）

void main() {
    // 全局缩放（sceneProps.x）：与球/键/网格一致（线框/点模式同场景同比例）
    const vec3 worldPos = aPos.xyz * ubo.sceneProps.x;
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vColor = aColor;
    vDist = length(worldPos - ubo.cameraPos.xyz);
}
