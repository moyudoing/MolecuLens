// =============================================================================
// MolecuLens · shaders/rt_tri.rchit —— 最近命中着色（三角网格 + 程序化球/胶囊）
// 规格来源：任务 E（GPU 光追模块）；光照与 CPU shade() 同构（rt_common.glsl）
// 说明：同一个 rchit 被两个 hit group 复用：
//         gl_InstanceCustomIndexEXT == 2 → 三角 BLAS（法线重心插值 + 逐三角材质）
//         否则                          → AABB 程序化几何（解析法线 + 图元材质）
//       程序化几何记录在 SSBO，命中点由 gl_HitTEXT 反解。
// =============================================================================
#version 460
#extension GL_GOOGLE_include_directive : require
#define MOLECULENS_RT_INCOMING_PAYLOAD 1   // 主载荷入向（写回命中结果）
#define MOLECULENS_RT_CLOSEST_HIT 1        // 阴影载荷出向（发射软阴影射线）
#include "rt_common.glsl"

hitAttributeEXT vec2 gHitAttrib;   // 仅程序化几何由 rint 写；三角形这里不用

void main() {
    const int custom = gl_InstanceCustomIndexEXT;
    const vec3 rayOrigin = gl_WorldRayOriginEXT;
    const vec3 rayDir = gl_WorldRayDirectionEXT;
    const vec3 P = rayOrigin + rayDir * gl_HitTEXT;

    vec3 N;
    vec3 base;
    vec3 ambientC;
    vec3 specularC;
    float shininess;
    float roughness;
    float metallic;
    float opacity;

    if (custom == 2) {
        // —— 三角网格：顶点法线重心插值（直接由命中点算重心，不依赖 attribute）——
        const uint tri = uint(gl_PrimitiveID);
        const RtTriRec rec = tris[tri];
        const RtVertex v0 = verts[rec.idx.x];
        const RtVertex v1 = verts[rec.idx.y];
        const RtVertex v2 = verts[rec.idx.z];
        const RtMatRec m = triMats[rec.idx.w];
        N = rtTriangleNormal(P, v0.pos.xyz, v1.pos.xyz, v2.pos.xyz,
                             v0.nrm.xyz, v1.nrm.xyz, v2.nrm.xyz);
        base = m.color.rgb;
        ambientC = m.ambient.rgb;
        specularC = m.specular.rgb;
        shininess = m.params.x;
        roughness = m.params.y;
        metallic = m.params.z;
        opacity = m.params.w;
    } else {
        // —— 程序化球/胶囊：命中点 + 图元参数 → 解析法线（与 CPU 求交同一公式）——
        const int base0 = (custom == 0) ? 0 : int(ubo.counts.x + 0.5);
        const RtPrimRec pr = prims[base0 + gl_PrimitiveID];
        if (custom == 0) {
            N = P - pr.geom0.xyz;
            const float len = length(N);
            N = len > 1e-8 ? N / len : vec3(0.0, 1.0, 0.0);
        } else {
            const vec3 a = pr.geom0.xyz;
            const vec3 b = pr.geom1.xyz;
            const vec3 axis = b - a;
            const float len = length(axis);
            if (len > 1e-8) {
                const vec3 au = axis / len;
                const float u = clamp(dot(P - a, au), 0.0, len);
                N = P - (a + au * u);   // 圆柱面与球帽统一：到轴最近点的方向
            } else {
                N = P - a;
            }
            const float nlen = length(N);
            N = nlen > 1e-8 ? N / nlen : vec3(0.0, 1.0, 0.0);
        }
        base = pr.color.rgb;
        ambientC = pr.ambient.rgb;
        specularC = pr.specular.rgb;
        shininess = pr.params.x;
        roughness = pr.params.y;
        metallic = pr.params.z;
        opacity = pr.params.w;
    }

    const uint seed = gPayload.seed;
    // 直接光照（软阴影射线由 rtShadeSurface 内部发射）
    const vec3 col = rtShadeSurface(N, base, ambientC, specularC, shininess,
                                    roughness, metallic, P, rayDir, seed);

    gPayload.color = col;
    gPayload.alpha = clamp(opacity, 0.0, 1.0);
    gPayload.hitPos = P;
    gPayload.t = gl_HitTEXT;
    gPayload.hit = 1;
}
