// =============================================================================
// MolecuLens · shaders/cylinder.frag —— 实例化键圆柱（同 sphere.frag 光照模型）
// 规格来源：主方案 §6.6.5 着色管线
// 光照/雾的单一定义处：material_shading.glsl（此前本文件另抄一份且雾色不同）
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
