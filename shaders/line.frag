// =============================================================================
// MolecuLens · shaders/line.frag —— 线段（无光照，配色直出 + 统一雾处理）
// 规格来源：主方案 §5.7.4
// 线框/坐标轴/高亮不做光照（line.vert 只输出顶点色），但雾必须与球/键/网格
// 一致 —— 此前本文件完全不处理雾，远处线框在开雾场景里不衰减。
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vDist;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(mix(vColor.rgb, fogColor(), depthCueFactor(vDist)), vColor.a);
}
