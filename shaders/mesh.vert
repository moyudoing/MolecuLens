// =============================================================================
// MolecuLens · shaders/mesh.vert —— 三角网格（卡通/表面 TriMesh）
// 规格来源：主方案 §5.3 TriMeshVertex（48B：pos4 + normal4 + color4）
// =============================================================================

#version 450

layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aNormal;
layout(location = 2) in vec4 aColor;

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
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vPos;
layout(location = 3) out float vDist;

void main() {
    gl_Position = ubo.viewProj * vec4(aPos.xyz, 1.0);
    vColor = aColor;
    vNormal = aNormal.xyz;
    vPos = aPos.xyz;
    vDist = length(aPos.xyz - ubo.cameraPos.xyz);
}
