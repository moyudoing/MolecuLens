// =============================================================================
// MolecuLens · shaders/global_ubo.glsl —— 全局 UBO 布局（set 0, binding 0）
// 规格来源：主方案 §6.6.4 光照 / §4.4 SceneProperties
// 所有分子渲染着色器共享此布局（通过 #include 或手工同步）。
// 对应 C++ 结构：engine/vulkan/realtime/vulkan_renderer.cpp GlobalUBO
// =============================================================================

// viewProj        : mat4   (0-64)
// cameraPos       : vec4   (64-80)
// lightDir        : vec4   (80-96)   指向光源方向（-dir）
// lightColor      : vec4   (96-112)
// ambient         : vec4   (112-128) rgb = 环境光色, w = 强度
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

// Blinn-Phong + metallic/roughness 高光缩放（主方案 §6.6.5）
vec3 shadeBlinnPhong(GlobalUBO ubo, PushMaterial mat, vec3 N, vec3 V, vec3 P, vec3 baseColor) {
    vec3 L = normalize(-ubo.lightDir.xyz);
    vec3 H = normalize(L + V);
    float ndl = max(dot(N, L), 0.0);
    float ndh = max(dot(N, H), 0.0);
    float spec = pow(ndh, max(mat.shininess, 0.001)) * (1.0 - mat.metallic);
    vec3 ambient  = ubo.ambient.rgb * ubo.ambient.w * baseColor;
    vec3 diffuse  = baseColor * ubo.lightColor.rgb * ndl;
    vec3 specular = mat.specular.rgb * spec * ubo.lightColor.rgb;
    return ambient + diffuse + specular;
}
