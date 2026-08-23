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
    vec3 base = mat.matDiffuse.rgb * vColor.rgb;
    // 全局光照（无方向光/点光源）：半球环境光 mix(地面色, 天顶色, N.y) + 均匀环境光
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);              // 0=朝下 1=朝上
    vec3 hemi = mix(ubo.lightColor.rgb, ubo.lightDir.rgb, up); // 地面色→天顶色
    vec3 col = base * (ubo.ambient.rgb + hemi * ubo.ambient.w);

    float alpha = mat.matOpacity * vColor.a;
    outColor = vec4(col, alpha);
}
