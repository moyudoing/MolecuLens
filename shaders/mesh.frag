// =============================================================================
// MolecuLens · shaders/mesh.frag —— 三角网格（同 sphere.frag 光照模型 + 雾效）
// 规格来源：主方案 §6.6.5
// 此前本文件**没有雾处理**，与 sphere/cylinder 不一致（同一场景三种雾行为）——
// 现统一走 global_ubo.glsl 的 fogFactor/fogColor。
// =============================================================================

#version 450
#extension GL_GOOGLE_include_directive : require
#include "material_shading.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vPos;
layout(location = 3) in float vDist;

layout(location = 0) out vec4 outColor;

void main() {
    if (clipDiscard(vPos)) {
        discard;
    }
    // 双面网格（ESP 表面/等值面/快速表面 cull=NONE，正反两面都会进片元着色器）：
    // 背面片元的 vNormal 朝外，直接拿去点乘光源会得到负值 → 半球环境光取到地面色、
    // 方向光完全消失，于是从壳外看进去是一块块暗斑。这里按 gl_FrontFacing 翻转背面法线，
    // 让内外两侧都按"面对观察者的一侧"受光；透明度与前后混合顺序不变。
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) {
        N = -N;
    }
    vec3 base = mat.matDiffuse.rgb * vColor.rgb;
    // 剖切时，朝内的背面就是"切口内壁"：给一点米色暖调，避免剖开后
    // 内部和外表面同色、看不出截面（参考图里切面是米色）。仅剖切开启时生效，
    // 不影响正常双面表面（ESP/等值面）的半透明观感。
    if (!gl_FrontFacing && ubo.clipParams.x > 0.5) {
        base = mix(base, vec3(0.82, 0.74, 0.58), 0.72);
    }

    vec3 col = shadeSurface(N, base, vPos);
    col = mix(col, fogColor(), depthCueFactor(vDist));

    float alpha = mat.matOpacity * vColor.a;
    // 玻璃壳（Glass 观感）：掠射角处光程变长 → 吸收更多（边缘更暗）、反射更强
    // （高光更亮），同时不透明度升高，形成清晰的薄壳轮廓。正面则更通透。
    // 只有半透明片元启用；不透明片元（球棍/线框）alpha=1 完全不受影响。
    if (alpha < 0.95) {
        const vec3 V = normalize(ubo.cameraPos.xyz - vPos);
        const float ndv = clamp(dot(N, V), 0.0, 1.0);
        const float fres = pow(1.0 - ndv, 3.0);
        const float rim = pow(1.0 - ndv, 6.0);
        alpha = clamp(alpha + (1.0 - alpha) * fres * 0.90, 0.0, 1.0);
        col *= mix(1.0, 0.72, fres);                              // 边缘吸收偏暗
        col += vec3(0.60, 0.72, 0.90) * (rim * 0.18);             // 冷调反射底
        col += mat.matSpecular.rgb * (rim * 0.55);                // 掠射高光（边缘亮边）
    }

    outColor = vec4(col, alpha);
}
