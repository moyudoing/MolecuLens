// =============================================================================
// MolecuLens · shaders/mesh.vert —— 三角网格（卡通/表面 TriMesh）
// 规格来源：主方案 §5.3 TriMeshVertex（48B：pos4 + normal4 + color4）
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aNormal;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vPos;
layout(location = 3) out float vDist;

void main() {
    // 全局缩放（sceneProps.x）：均匀缩放不改法线
    const vec3 worldPos = aPos.xyz * ubo.sceneProps.x;
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vColor = aColor;
    vNormal = aNormal.xyz;
    vPos = worldPos;
    vDist = length(worldPos - ubo.cameraPos.xyz);
}
