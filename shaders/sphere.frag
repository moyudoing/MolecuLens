// =============================================================================
// MolecuLens · shaders/sphere.frag —— 实例化原子球（统一光照 + 雾效 + 透明）
// 规格来源：主方案 §6.6.5 着色管线 / §6.6.1 材质模型 / §6.6.4 光照
// 光照与雾的实现在 material_shading.glsl / global_ubo.glsl（单一定义处）。
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "material_shading.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vPos;
layout(location = 3) in vec4 vPickId;
layout(location = 4) in float vDist;

layout(location = 0) out vec4 outColor;

void main() {
    if (clipDiscard(vPos)) {
        discard;
    }
    const vec3 N = normalize(vNormal);
    const vec3 base = mat.matDiffuse.rgb * vColor.rgb;

    vec3 col = shadeSurface(N, base, vPos);
    col = mix(col, fogColor(), depthCueFactor(vDist));

    outColor = vec4(col, mat.matOpacity * vColor.a);
}
