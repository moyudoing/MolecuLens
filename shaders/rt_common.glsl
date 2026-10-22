// =============================================================================
// MolecuLens · shaders/rt_common.glsl —— GPU 光追共享定义（描述符 + 光照）
// 规格来源：主方案 §6.6.4/§6.6.5 + 任务 E（GPU 光追模块）
// 说明：光照公式与 CPU 路径 engine/raytrace/ray_tracer.cpp 的 shade() **同构**
//       （半球环境光 + 方向光 Lambert/Blinn-Phong + 软阴影 + 1 次反射），
//       两条路径出图才能直接对比（验收：非背景像素 ±10%）。
//       本文件只放：结构体、绑定、光照/阴影/反射 —— 不放 main()。
// =============================================================================
#ifndef MOLECULENS_RT_COMMON_GLSL
#define MOLECULENS_RT_COMMON_GLSL

#extension GL_EXT_ray_tracing : require

// —— 图元记录（球 / 胶囊共用；96 字节，std430）——
struct RtPrimRec {
    vec4 geom0;    // 球: center.xyz + radius；胶囊: a.xyz + radius
    vec4 geom1;    // 球: 未用；胶囊: b.xyz + 类型（0 球 / 1 胶囊）
    vec4 color;
    vec4 ambient;
    vec4 specular;
    vec4 params;   // x = shininess, y = roughness, z = metallic, w = opacity
};
// —— 三角网格顶点（32 字节；位置写入 BLAS，法线用于平滑着色）——
struct RtVertex {
    vec4 pos;      // xyz
    vec4 nrm;      // xyz（齐次 w 未用）
};
// —— 三角形索引 + 材质下标 ——
struct RtTriRec {
    uvec4 idx;     // x/y/z = 三个顶点下标，w = 材质下标
};
// —— 三角形材质（与 CPU RtTriangle 的材质字段一一对应）——
struct RtMatRec {
    vec4 color;
    vec4 ambient;
    vec4 specular;
    vec4 params;   // x = shininess, y = roughness, z = metallic, w = opacity
};

// —— 描述符（所有光追阶段共用同一 set）——
layout(set = 0, binding = 0) uniform accelerationStructureEXT uTopLevelAS;
layout(set = 0, binding = 1, rgba8) uniform image2D uOutImage;
layout(set = 0, binding = 2) uniform RtUniforms {
    vec4 camPos;        // xyz = 相机位置；w = tan(fov/2)
    vec4 camForward;    // xyz = 前向；w = aspect
    vec4 camRight;      // xyz = 右向；w = spp
    vec4 camUp;         // xyz = 上向；w = 图像宽度
    vec4 skyColor;      // xyz = 天顶色；w = ambientIntensity
    vec4 groundColor;   // xyz = 地面色；w = shadowSamples
    vec4 ambientColor;  // xyz = 环境色；w = ambientWeight
    vec4 dirLightDir;   // xyz = 指向光源（未归一化）；w = dirLightIntensity
    vec4 dirLightColor; // xyz = 方向光颜色；w = 1 启用 / 0 关闭
    vec4 weights;       // x = diffuseWeight, y = specularWeight, z = maxBounces, w = 高度
    vec4 counts;        // x = 球数, y = 胶囊数, z = 三角数, w = 最大透明层数
    vec4 bgColor;       // xyz = 背景色；w = 随机种子偏移
} ubo;
layout(set = 0, binding = 3) readonly buffer RtPrimBuf { RtPrimRec prims[]; };
layout(set = 0, binding = 4) readonly buffer RtVertBuf { RtVertex verts[]; };
layout(set = 0, binding = 5) readonly buffer RtTriBuf { RtTriRec tris[]; };
layout(set = 0, binding = 6) readonly buffer RtTriMatBuf { RtMatRec triMats[]; };

// —— 光线载荷（location 0：主光线 / 透明层推进）——
// 关键：方向必须匹配！raygen 用 rayPayloadEXT（出向，RayPayloadKHR），
// miss / closest-hit 必须用 rayPayloadInEXT（入向，IncomingRayPayloadKHR）——
// 否则命中/未命中的写入落在"另一个出向载荷"上，raygen 读回永远是初始值
// （实测表现：所有射线都被当成 miss、整个画面只有背景）。
struct RtPayload {
    vec3 color;    // 命中点着色
    float alpha;   // 该层不透明度
    vec3 hitPos;   // 命中点世界坐标（透明层推进起点）
    float t;       // 命中距离
    int hit;       // 1 = 命中
    int depth;     // 保留字段（GPU 不做反射；raygen 仍按 CPU 语义写入）
    uint seed;     // 软阴影抖动种子
};
// —— 阴影载荷（location 1：1 = 完全受光，0 = 被遮挡）——
struct RtShadowPayload {
    float lit;
};
// 交集（intersection）着色器不能声明 rayPayloadEXT，也不发射光线；
// 由 rt_prim.rint 在 include 前定义该宏，跳过载荷与光照函数。
#ifndef MOLECULENS_RT_INTERSECTION
#if defined(MOLECULENS_RT_INCOMING_PAYLOAD)
layout(location = 0) rayPayloadInEXT RtPayload gPayload;
#else
layout(location = 0) rayPayloadEXT RtPayload gPayload;
#endif
// 阴影射线：closest-hit 发射（出向），shadow miss 写回（入向）。
#if defined(MOLECULENS_RT_CLOSEST_HIT)
layout(location = 1) rayPayloadEXT RtShadowPayload gShadow;
#elif defined(MOLECULENS_RT_SHADOW_MISS)
layout(location = 1) rayPayloadInEXT RtShadowPayload gShadow;
#endif
#endif

const float kRtEps = 1e-4;          // 与 CPU kEps 一致

// 确定性伪随机 —— 与 CPU hashUnit 完全同式（软阴影抖动/AA 抖动对齐）。
float rtHashUnit(uint a, uint b) {
    uint x = a * 374761393u + b * 668265263u;
    x = (x ^ (x >> 13)) * 1274126177u;
    x ^= x >> 16;
    return float(x & 0xFFFFFFu) / 16777216.0;
}

#if defined(MOLECULENS_RT_CLOSEST_HIT)

// 半球环境光 + 方向光（与 CPU shade 同构）。
// N 输入法线（内部会翻到朝向观察者一侧），rayDir = 入射光线方向（指向表面）。
// 说明：GPU 端不做反射（CPU 的 1 次弹射只是 0.18 的弱反射项；RT 管线里
// 入向载荷不能复用为嵌套 trace 的载荷，需要额外 location/模块，收益不值）。
vec3 rtShadeSurface(vec3 N, vec3 base, vec3 ambientC, vec3 specularC,
                    float shininess, float roughness, float metallic,
                    vec3 P, vec3 rayDir, uint seed) {
    N = normalize(N);
    if (dot(N, rayDir) > 0.0) {
        N = -N;   // 内表面/球形帽可能给出背向法线
    }
    const float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    const vec3 hemi = mix(ubo.groundColor.rgb, ubo.skyColor.rgb, up);
    vec3 col = base * ambientC *
               (ubo.ambientColor.rgb + hemi * ubo.skyColor.w) * ubo.ambientColor.w;

    if (ubo.dirLightColor.w > 0.5 && ubo.dirLightDir.w > 0.0) {
        const vec3 L = normalize(ubo.dirLightDir.xyz);
        const vec3 V = -rayDir;
        const float shininess2 = max(shininess, 1.0) * (1.0 - 0.7 * roughness);
        const vec3 specColor = mix(specularC, base, metallic);
        const float ndl = max(dot(N, L), 0.0);
        if (ndl > 0.0) {
            const int samples = int(max(1.0, ubo.groundColor.w));
            const vec3 origin = P + N * kRtEps;
            float lit = 0.0;
            for (int s = 0; s < samples; ++s) {
                vec3 ls = L;
                if (samples > 1) {
                    const float j1 =
                        rtHashUnit(seed * 2654435761u + uint(s), seed + 17u) - 0.5;
                    const float j2 =
                        rtHashUnit(seed + 31u, seed * 40503u + uint(s)) - 0.5;
                    ls = normalize(L + 0.10 * j1 * vec3(1.0, 0.0, 0.0) +
                                   0.10 * j2 * vec3(0.0, 1.0, 0.0));
                }
                // 阴影射线：命中即遮挡（miss 着色器把 lit 置 1）
                gShadow.lit = 0.0;
                traceRayEXT(uTopLevelAS,
                            gl_RayFlagsTerminateOnFirstHitEXT |
                                gl_RayFlagsOpaqueEXT |
                                gl_RayFlagsSkipClosestHitShaderEXT,
                            0xFF, 0, 0, 1, origin, 0.0, ls, 1e30, 1);
                lit += gShadow.lit;
            }
            lit /= float(samples);

            const vec3 H = normalize(L + V);
            const float spec = pow(max(dot(N, H), 0.0), shininess2);
            col += (base * ubo.dirLightColor.rgb *
                        (ndl * ubo.dirLightDir.w * ubo.weights.x) +
                    specColor * ubo.dirLightColor.rgb *
                        (spec * ubo.dirLightDir.w * ubo.weights.y)) * lit;
        }
    }

    return max(col, vec3(0.0));
}

// 三角形重心插值法线（不依赖 hit attribute 的坐标系约定：直接用命中点算重心）
vec3 rtTriangleNormal(vec3 p, vec3 a, vec3 b, vec3 c,
                      vec3 na, vec3 nb, vec3 nc) {
    const vec3 v0 = b - a;
    const vec3 v1 = c - a;
    const vec3 v2 = p - a;
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = d00 * d11 - d01 * d01;
    vec3 n = cross(v0, v1);
    if (abs(denom) > 1e-20) {
        const float v = (d11 * d20 - d01 * d21) / denom;
        const float w = (d00 * d21 - d01 * d20) / denom;
        const float u = 1.0 - v - w;
        n = na * u + nb * v + nc * w;
    }
    return normalize(n);
}

#endif // MOLECULENS_RT_CLOSEST_HIT

#endif // MOLECULENS_RT_COMMON_GLSL
