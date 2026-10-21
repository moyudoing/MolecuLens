// =============================================================================
// MolecuLens · shaders/material_shading.glsl —— 材质 push constant + 统一光照模型
// 规格来源：主方案 §5.3（MaterialBlock 80B）/ §6.6.1 材质模型 / §6.6.4 光照
//
// 仅**片元**着色器 include（含 push constant 声明；顶点着色器的管线布局里没有
// 对应 range，声明了会引入布局不匹配风险）。
// =============================================================================

#extension GL_GOOGLE_include_directive : require
#include "global_ubo.glsl"

// 材质 push constant（80B，与 C++ MaterialBlock 载荷逐字节对齐，主方案 §5.3）。
// 实例名 mat，与 shadeSurface 内的引用一致（同 fogFactor 引用 ubo 的做法）。
layout(push_constant) uniform PushMaterial {
    vec4 matAmbient;
    vec4 matDiffuse;
    vec4 matSpecular;
    float matShininess;
    float matMetallic;
    float matRoughness;
    float matOpacity;
    float matShadowMask;   // 0 = 不接收动态阴影（半透明表面），1 = 接收
    float matPad0, matPad1, matPad2;
} mat;

// 统一光照：半球环境光 + 方向光（环绕漫反射 + Blinn-Phong 高光）+ 掠射轮廓。
//   N     世界空间法线（单位）
//   base  漫反射底色（材质 diffuse × 顶点色）
//   P     世界空间片元位置（高光视线方向用）
//
// 与旧版的差异（v2.2 风格化，全部为**原实现**，不复制第三方着色器）：
//   1) 方向光漫反射加 wrap：明暗交界从"硬边"变成柔和过渡（styleParams.x）；
//   2) 摄像机侧补光：没有屏幕空间 AO 通道时，用 N·V 补一点正面光，避免背光
//      面死黑；这是"柔和棚拍"观感的主要来源；
//   3) 掠射角压暗：在球/柱/表面的轮廓处轻微压暗，替代后处理描边（styleParams.z）；
//   4) 掠射角补光：极弱的亮边，让轮廓在深色背景上仍可分辨（styleParams.y）。
vec3 shadeSurface(vec3 N, vec3 base, vec3 P) {
    const vec3 V = normalize(ubo.cameraPos.xyz - P);
    const float ndv = clamp(dot(N, V), 0.0, 1.0);
    const float wrap = clamp(ubo.styleParams.x, 0.0, 0.9);

    // 1) 半球环境光：天顶色↔地面色按法线朝向插值，再乘材质环境色
    const float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);    // 0=朝下 1=朝上
    const vec3 hemi = mix(ubo.lightColor.rgb, ubo.lightDir.rgb, up);
    vec3 col = base * mat.matAmbient.rgb * (ubo.ambient.rgb + hemi * ubo.ambient.w)
             * ubo.lightParams.x;

    // 2) 方向光（未启用时恒为 0）
    if (ubo.dirLightDir.w > 0.5) {
        const vec3 L = normalize(ubo.dirLightDir.xyz);
        const float ndlRaw = dot(N, L);
        // 环绕漫反射：wrap=0 退化为 max(N·L,0)，wrap 越大越"柔"
        float ndl = clamp((ndlRaw + wrap) / (1.0 + wrap), 0.0, 1.0);
        // 方向光阴影：可见度同时衰减漫反射与高光（被遮挡处不应出现高光）。
        // 半透明表面（ESP/等值面/快速表面，由 CPU 置 matShadowMask = 0）**不接收阴影**：
        //   · 它们自身是双面渲染，背面片元会被正面深度遮挡 → 壳内出现大片暗斑；
        //   · 壳内的球棍/原子还会把影子投到壳面上，形成与分子同形的黑色幽灵；
        //   · 半透明材质本来就该"透光"，把阴影整块压黑在物理上也不成立。
        const float vis = (mat.matShadowMask < 0.5)
                              ? 1.0
                              : shadowVisibility(P, max(ndlRaw, 0.0));
        ndl *= vis;
        const float shininess = max(mat.matShininess, 1.0) * (1.0 - 0.7 * mat.matRoughness);
        const vec3 specColor = mix(mat.matSpecular.rgb, base, mat.matMetallic);
        float spec = 0.0;
        if (ndlRaw > 0.0) {
            const vec3 H = normalize(L + V);
            spec = pow(max(dot(N, H), 0.0), shininess);
        }
        col += base * ubo.dirLightColor.rgb * (ndl * ubo.dirLightColor.w * ubo.lightParams.y)
             + specColor * ubo.dirLightColor.rgb
                   * (spec * ubo.dirLightColor.w * ubo.lightParams.z);
    }

    // 3) 摄像机侧补光：正面朝向观察者的片元获得一点半球色，模拟大面积柔光箱。
    //    强度刻意压得很低（0.10），只负责"把暗部提起来"，不改变配色。
    const float fill = ndv * ndv * 0.10 * ubo.lightParams.y;
    col += base * hemi * fill;

    // 4) 轮廓描边：用屏幕空间导数（fwidth）把"法线与视线接近垂直"的一圈压缩成
    //    约 1~2 像素的暗边 —— 不依赖后处理通道，任何分辨率下宽度稳定。
    //    这比 pow(1-N·V,k) 更接近"剪影/描边"的观感：球与键的边界是干净的细线，
    //    而不是随球面曲率扩散的暗块。内部重叠处的遮挡轮廓仍需 SSAO/描边后处理，
    //    这里只负责外轮廓。
    const float ndvWidth = max(fwidth(ndv) * 1.5, 1e-5);
    const float edgePx = 1.0 - smoothstep(0.0, ndvWidth, ndv);
    col *= (1.0 - 0.75 * ubo.styleParams.z * edgePx);
    col += base * (0.10 * ubo.styleParams.y * edgePx);

    return col;
}
