// =============================================================================
// MolecuLens · shaders/mesh.frag —— 三角网格（同 sphere.frag 光照模型）
// =============================================================================

#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vPos;
layout(location = 3) in float vDist;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
} ubo;

layout(push_constant) uniform PushMaterial {
    vec4 matAmbient;
    vec4 matDiffuse;
    vec4 matSpecular;
    float matShininess;
    float matMetallic;
    float matRoughness;
    float matOpacity;
} mat;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(ubo.cameraPos.xyz - vPos);
    vec3 L = normalize(-ubo.lightDir.xyz);
    vec3 H = normalize(L + V);

    float ndl = max(dot(N, L), 0.0);
    float ndh = max(dot(N, H), 0.0);
    float spec = pow(ndh, max(mat.matShininess, 0.001)) * (1.0 - mat.matMetallic);

    vec3 base = mat.matDiffuse.rgb * vColor.rgb;
    vec3 col = ubo.ambient.rgb * ubo.ambient.w * base
             + base * ubo.lightColor.rgb * ndl
             + mat.matSpecular.rgb * spec * ubo.lightColor.rgb;

    float alpha = mat.matOpacity * vColor.a;
    outColor = vec4(col, alpha);
}
