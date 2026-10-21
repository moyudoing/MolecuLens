// =============================================================================
// MolecuLens · shaders/point.frag —— 点模式片元（无光照，纯色点 + 统一雾处理）
// 规格来源：主方案 §5.7.4（点模式）；雾与 line.frag 一致，见其注释。
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vDist;
layout(location = 2) in vec3 vPos;

layout(location = 0) out vec4 outColor;

void main() {
    if (clipDiscard(vPos)) {
        discard;
    }
    // 方形 point sprite → 圆形点：超出单位圆丢弃，内部做一点球面明暗，
    // 否则点模式看起来是一堆方块（此前完全没处理 gl_PointCoord）。
    const vec2 p = gl_PointCoord * 2.0 - 1.0;
    const float r2 = dot(p, p);
    if (r2 > 1.0) {
        discard;
    }
    const vec3 N = vec3(p.x, -p.y, sqrt(max(1.0 - r2, 0.0)));
    const float shade = 0.72 + 0.28 * clamp(N.z, 0.0, 1.0);
    const vec3 col = mix(vColor.rgb * shade, fogColor(), depthCueFactor(vDist));
    outColor = vec4(col, vColor.a);
}
