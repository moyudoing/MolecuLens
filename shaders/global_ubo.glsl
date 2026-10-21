// =============================================================================
// MolecuLens · shaders/global_ubo.glsl —— 全局 UBO / 雾 的**唯一定义**
// 规格来源：主方案 §6.6.4 光照 / §4.4 SceneProperties
//
// 本文件是 GLSL 共享头（不单独编译），由各着色器经
//     #extension GL_GOOGLE_include_directive : require
//     #include "global_ubo.glsl"
// 引入。此前 10 个着色器各自手抄一份 uniform block，任何字段增删都要逐文件改，
// 漏一处即"UBO 与 GLSL 布局错位"（主方案 §12 列为主要风险）。收敛到本文件后，
// 布局只在两处定义：此处（GLSL）与 engine/vulkan/desc/descriptors.h（C++，
// 带 static_assert 尺寸校验）。
//
// ⚠ 字段顺序/偏移必须与 C++ 侧逐字节一致。前 160B 为定稿布局（勿调整顺序）；
//   方向光字段一律追加在末尾。
// =============================================================================

// viewProj        : mat4   (0-64)
// cameraPos       : vec4   (64-80)    相机世界坐标（高光项视线方向用）
// lightDir        : vec4   (80-96)    半球光：rgb = 天顶色
// lightColor      : vec4   (96-112)   半球光：rgb = 地面色
// ambient         : vec4   (112-128)  rgb = 环境光基础色, w = 半球光强度
// fog             : vec4   (128-144)  x near, y far, z mode(0关/1线性/2指数/3指数²), w density
// sceneProps      : vec4   (144-160)  x = 全局缩放, yzw 保留
// dirLightDir     : vec4   (160-176)  xyz = 方向（指向光源，已归一化）, w = 启用(0/1)
// dirLightColor   : vec4   (176-192)  rgb = 颜色, w = 强度
// lightParams     : vec4   (192-208)  x = 环境权重, y = 漫反射权重, z = 高光权重, w = 阴影强度
// lightViewProj   : mat4   (208-272)  光源视图投影（正交；深度 pass 与阴影查表用）
// shadowParams    : vec4   (272-288)  x = 阴影贴图边长, y = 深度偏移, zw 保留
// styleParams     : vec4   (288-304)  x=漫反射环绕 y=掠射亮边 z=掠射压暗 w=距离淡出
// backgroundColor : vec4   (304-320)  rgb = 清屏/雾目标色（线性空间）
// clipPlane       : vec4   (320-336)  xyz = 平面法线, w = 常数 d（dot(p,n)+d=0）
// clipParams      : vec4   (336-352)  x = 剖切启用(0/1)
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
    vec4 dirLightDir;
    vec4 dirLightColor;
    vec4 lightParams;
    mat4 lightViewProj;
    vec4 shadowParams;
    vec4 styleParams;
    vec4 backgroundColor;
    vec4 clipPlane;
    vec4 clipParams;
} ubo;

// 剖切面（v2.3）：世界空间平面 dot(p,n)+d=0，丢弃正侧片元。返回 true = discard。
// 只对有世界坐标的图元（球/柱/网格）生效；线/点不参与剖切（轴与线框保持完整）。
bool clipDiscard(vec3 worldPos) {
    if (ubo.clipParams.x < 0.5) {
        return false;
    }
    return dot(worldPos, ubo.clipPlane.xyz) + ubo.clipPlane.w > 0.0;
}

// 雾效（片元着色器通用）：dist = 该片元到相机的世界距离
float fogFactor(float dist) {
    if (ubo.fog.z < 0.5) return 0.0;                       // 关
    float f;
    if (ubo.fog.z < 1.5) {                                 // 线性
        f = clamp((dist - ubo.fog.x) / max(ubo.fog.y - ubo.fog.x, 1e-5), 0.0, 1.0);
    } else if (ubo.fog.z < 2.5) {                          // 指数
        f = 1.0 - exp(-dist * ubo.fog.w);
    } else {                                               // 指数²
        float d = dist * ubo.fog.w;
        f = 1.0 - exp(-d * d);
    }
    return clamp(f, 0.0, 1.0);
}

// 统一雾色。此前 sphere 用 ambient.rgb*0.15、cylinder 用 vec3(0.05)、mesh 完全没有
// 雾 —— 同一场景三种雾处理。收敛为这一个常量，全部着色器共用。
// 现在改为**背景色**：距离淡出必须淡向画布颜色，否则远景会淡成一块与背景
// 不同的灰雾（白底出图尤其明显）。背景色由后端按实际清屏值写入 UBO。
vec3 fogColor() {
    return ubo.backgroundColor.rgb;
}

// 距离淡出（雾）强度，已乘上风格参数：w=0 时完全关闭（出版预设），
// w=1 时等价于原 fogFactor。line/point 等无光照图元也走这里。
float depthCueFactor(float dist) {
    return clamp(fogFactor(dist) * ubo.styleParams.w, 0.0, 1.0);
}

// —— 方向光阴影（主方案 §6.6.4；深度 pass + PCF）——
// binding 1 = 阴影深度图（由 ShadowMap 绑定；未启用阴影时绑的是 1×1 占位图，
// 此时 ubo.lightParams.w == 0，下面直接返回 1.0 不采样）。
layout(set = 0, binding = 1) uniform sampler2D uShadowMap;

// 世界坐标 → 阴影贴图 UV 与参考深度。
// 光源视图投影是正交的，故 w 恒为 1，无需透视除法补偿。
// 返回 false 表示该点在光源视锥之外（视为受光，避免范围外出现假阴影）。
bool shadowCoord(vec3 worldPos, out vec2 uv, out float refDepth) {
    const vec4 lc = ubo.lightViewProj * vec4(worldPos, 1.0);
    const vec3 ndc = lc.xyz / lc.w;
    uv = ndc.xy * 0.5 + 0.5;
    refDepth = ndc.z;                                  // Vulkan: NDC z ∈ [0,1]
    return uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0 && refDepth < 1.0;
}

// 3×3 PCF：受光返回 1，完全遮挡返回 1-阴影强度。
// 偏移量按**纹素**算（shadowParams.x = 贴图边长），与分辨率解耦 —— 写死 UV 偏移
// 会在换贴图尺寸时改变半影宽度。
float shadowVisibility(vec3 worldPos, float ndl) {
    if (ubo.lightParams.w <= 0.0 || ubo.shadowParams.x <= 0.0) {
        return 1.0;   // 未启用阴影
    }
    vec2 uv;
    float refDepth;
    if (!shadowCoord(worldPos, uv, refDepth)) {
        return 1.0;   // 视锥外：不受影
    }
    // 斜率缩放偏移：掠射角下深度误差大，固定偏移会产生"条纹状"自阴影（shadow acne）
    const float bias = max(0.0015 * (1.0 - ndl), 0.0004);
    const float texel = 1.0 / ubo.shadowParams.x;
    float lit = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const float d = texture(uShadowMap,
                                    uv + vec2(float(dx), float(dy)) * texel).r;
            lit += (refDepth - bias <= d) ? 1.0 : 0.0;
        }
    }
    lit /= 9.0;
    return mix(1.0 - ubo.lightParams.w, 1.0, lit);
}
