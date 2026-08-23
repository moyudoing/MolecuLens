// =============================================================================
// MolecuLens · shaders/global_ubo.glsl —— 全局 UBO 布局（set 0, binding 0）
// 规格来源：主方案 §6.6.4 光照 / §4.4 SceneProperties
// 所有分子渲染着色器共享此布局（通过 #include 或手工同步）。
// 对应 C++ 结构：engine/vulkan/realtime/vulkan_renderer.cpp GlobalUBO
// =============================================================================

// viewProj        : mat4   (0-64)
// cameraPos       : vec4   (64-80)
// lightDir        : vec4   (80-96)   全局光：rgb = 天顶色（半球环境光）
// lightColor      : vec4   (96-112)  全局光：rgb = 地面色（半球环境光）
// ambient         : vec4   (112-128) rgb = 均匀环境光基础色, w = 半球光强度
// fog             : vec4   (128-144) x = near, y = far, z = mode(0关/1线性/2指数/3指数²), w = density
// sceneProps      : vec4   (144-160) x = 全局缩放, yzw 保留
struct GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
};

// 材质 push constant（64B，与 MaterialBlock 载荷对齐，主方案 §5.3）
struct PushMaterial {
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    float shininess;
    float metallic;
    float roughness;
    float opacity;
};

// 雾效计算（片元着色器通用）
float fogFactor(GlobalUBO ubo, float dist) {
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

// 全局光照（无方向光/点光源）：半球环境光 mix(地面色, 天顶色, N.y) + 均匀环境光。
// 各片元着色器内联实现（GLSL 无 include）。
vec3 shadeGlobal(GlobalUBO ubo, vec3 N, vec3 baseColor) {
    float up = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);              // 0=朝下 1=朝上
    vec3 hemi = mix(ubo.lightColor.rgb, ubo.lightDir.rgb, up); // 地面色→天顶色
    return baseColor * (ubo.ambient.rgb + hemi * ubo.ambient.w);
}
