// =============================================================================
// MolecuLens · engine · render/render_parameter.h —— 渲染参数
// 规格来源：主方案 §5.7.2 / §7.7（BondRenderParams::computeEndpoints：
//           圆柱键端面沿键轴向内缩使端面圆周与原子球面相切；
//           勾股定理 d² + cylR² = sphereR²；防御：偏移 ≤ 键长 40%、
//           极短键按比例收缩、退化 0.49 兜底）
// =============================================================================

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace moleculens::engine {

// 阴影贴图边长（2 的幂）——**两个 Vulkan 后端共用的唯一定义**。
// 实时/离屏渲染器都按此创建深度图；LightParams::shadowMapSize 必须与之一致
// （着色器用它换算 PCF 纹素偏移 shadowParams.x）。此前两个后端各硬编码 2048，
// 改尺寸要同时改三处、漏一处就出现半影宽度不一致 —— 收敛到本常量。
inline constexpr int kDefaultShadowMapSize = 2048;

// 光照参数（半球环境光 + 方向光；主方案 §6.6.4 光照）。
// 全局 UBO 光照来源：RenderView 携带 → 各后端统一从 view 构建（而非各自硬编码）。
// 语义与着色器 GlobalUBO 匹配：skyColor → lightDir 槽（天顶色）、groundColor →
// lightColor 槽（地面色）、ambientColor+w → ambient 槽（基础色+半球光强度）、
// 方向光三项 → 末尾新增槽（见 descriptors.h GlobalUBO）。
struct LightParams {
    glm::vec3 skyColor{1.0f, 1.0f, 1.0f};         // 天顶色（半球上方）
    glm::vec3 groundColor{0.35f, 0.4f, 0.5f};     // 地面色（半球下方）
    glm::vec3 ambientColor{0.55f, 0.6f, 0.75f};   // 均匀环境光基础色
    float ambientIntensity = 0.45f;               // 半球光强度

    // —— 方向光（默认开启，强度保守：半球环境光仍为主光，方向光提供造型与高光）——
    glm::vec3 dirLightDir{0.40f, 0.80f, 0.60f};   // 方向（指向光源；着色器内归一化）
    glm::vec3 dirLightColor{1.0f, 0.98f, 0.95f};  // 颜色（略暖）
    float dirLightIntensity = 0.35f;              // 强度（0 = 关）
    bool  dirLightEnabled = true;

    // 三项权重（1 = 全强度；UI 可调，用于压暗某一项做诊断/风格化）
    float ambientWeight = 1.0f;
    float diffuseWeight = 1.0f;
    float specularWeight = 1.0f;

    // —— 方向光阴影（主方案 §6.6.4；深度 pass + PCF）——
    // 实际阴影贴图尺寸由两个后端共用 kDefaultShadowMapSize 创建；此处的 size 用于
    // 着色器里的纹素偏移（shadowParams.x），两者必须一致才有正确的半影宽度。
    bool  shadowsEnabled = true;
    float shadowStrength = 0.65f;   // 阴影强度（0 = 无阴影，1 = 全黑本影）
    int   shadowMapSize = kDefaultShadowMapSize;   // 阴影贴图边长（与后端一致）
    // 光源视图投影（正交，按场景包围球拟合）。由 Renderer::sceneLighting 计算。
    // 未设置（全零）时着色器不做阴影查表 —— 保证"无场景/无方向光"时不误判。
    glm::mat4 lightViewProj{0.0f};

    static LightParams defaults() { return {}; }

    // 由方位角/仰角（弧度）设置方向光方向（UI 滑杆用）。
    // 约定：方位角绕 +Y 轴、从 +Z 起算；仰角 90° = 正上方。
    void setDirLightAngles(float azimuth, float elevation) {
        const float ce = std::cos(elevation);
        dirLightDir = glm::vec3(ce * std::sin(azimuth), std::sin(elevation),
                                ce * std::cos(azimuth));
        const float len = glm::length(dirLightDir);
        if (len > 1e-6f) {
            dirLightDir /= len;
        }
    }

    // 方位角/仰角（弧度）——setDirLightAngles 的逆运算（UI 回读当前值）
    void dirLightAngles(float* azimuth, float* elevation) const {
        const glm::vec3 d = glm::normalize(dirLightDir);
        *elevation = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        *azimuth = std::atan2(d.x, d.z);
    }
};

// =============================================================================
// 渲染风格（v2.2「柔和光 / 出版级」外观的显式参数）
// =============================================================================
// 目标不是复制某个体渲染器的实现，而是把"教科书插图式"的观感拆成四个可控
// 自由度，让 GUI 预设与 CLI 出图共用同一套语义：
//   wrap     方向光漫反射的环绕量（0 = 硬明暗交界；0.3~0.6 = 柔和过渡）
//   rim      掠射角补光强度（把物体边缘从背景里"提"出来）
//   outline  掠射角压暗强度（无后处理通道时的轮廓/描边近似）
//   depthCue 距离淡出倍率（1 = 完全按雾参数淡向背景色；0 = 不淡出）
// 全部为**线性空间**系数，写进 GlobalUBO.styleParams（见 descriptors.h / 
// shaders/global_ubo.glsl），不参与命令流重建，逐帧调参无重建开销。
struct RenderStyle {
    float wrap = 0.35f;
    float rim = 0.18f;
    float outline = 0.32f;
    float depthCue = 1.0f;

    bool operator==(const RenderStyle& o) const {
        return wrap == o.wrap && rim == o.rim && outline == o.outline &&
               depthCue == o.depthCue;
    }
    bool operator!=(const RenderStyle& o) const { return !(*this == o); }
};

enum class RenderStylePreset : uint8_t { Default = 0, Soft, Publication, Vivid };

// 预设的四个数值。调用方（GUI/CLI）可在预设基础上再微调。
inline RenderStyle renderStylePreset(RenderStylePreset preset) {
    switch (preset) {
        case RenderStylePreset::Soft:        // 柔和：大环绕 + 弱描边，接近 AO 观感
            return {0.55f, 0.14f, 0.14f, 1.0f};
        case RenderStylePreset::Publication: // 出版：白底 + 清晰描边 + 无景深淡出
            return {0.45f, 0.00f, 0.55f, 0.0f};
        case RenderStylePreset::Vivid:       // 鲜明：对比强、边缘亮
            return {0.18f, 0.30f, 0.38f, 1.0f};
        case RenderStylePreset::Default:
        default:
            return {};
    }
}

inline const char* renderStylePresetKey(RenderStylePreset preset) {
    switch (preset) {
        case RenderStylePreset::Soft: return "soft";
        case RenderStylePreset::Publication: return "publication";
        case RenderStylePreset::Vivid: return "vivid";
        case RenderStylePreset::Default:
        default: return "default";
    }
}

// 未知/空串 → Default（供 CLI 解析与设置持久化回读共用）
inline RenderStylePreset renderStylePresetFromKey(const std::string& key) {
    if (key == "soft") return RenderStylePreset::Soft;
    if (key == "publication") return RenderStylePreset::Publication;
    if (key == "vivid") return RenderStylePreset::Vivid;
    return RenderStylePreset::Default;
}

// =============================================================================
// 材质库（名称沿用经典材质库，见需求附图）
// =============================================================================
// 说明（务必如实告知使用者，故写在代码里）：
//   · **材质名与参考图逐项对齐**（Opaque … RTChrome 共 23 项）。
//   · **数值是按本引擎 Phong/PBR 管线观感调校的近似值，不是原始参数表**。
//     Edgy*/AO* 系材质依赖描边（outline）与环境光遮蔽渲染通道，本引擎
//     没有这两条通道，故以"高环境光 + 低高光"（AO 系）与"高光边 + 低环境光"（Edgy 系）
//     近似其观感；Glass/Bubble 系以低不透明度 + 强高光近似。
//   · 增删/重排本表会让基于**下标**的旧配置指向别的材质 —— 故 .mlscene 持久化
//     存的是**材质名**（见 scene_bridge），名字是稳定键，下标不是。
struct MaterialPreset {
    const char* name;
    float ambient[3];
    float diffuse[3];
    float specular[3];
    float shininess;   // 光泽度（Phong 指数；越大高光越锐）
    float metallic;    // 金属度（PBR 扩展）
    float roughness;   // 粗糙度（PBR 扩展）
    float opacity;     // 不透明度（<1 进入透明通道）
};

// 顺序与参考图一致（Opaque 为默认第一项）。
// ⚠ Opaque 的数值**必须与 Representation 的字段默认值逐项相同**（ambient 0.60 /
//   diffuse 0.90 / specular 0.30 / shininess 32 / metallic 0 / roughness 0.40 /
//   opacity 1.0）：这样"新表示 = Opaque"自洽，且引入材质库**不改变现有默认观感**
//   （否则全部既有截图/冒烟阈值都建立在另一套默认值上）。
inline const MaterialPreset kMaterialPresets[] = {
    {"Opaque",         {0.60f,0.60f,0.60f}, {0.90f,0.90f,0.90f}, {0.30f,0.30f,0.30f},  32.0f, 0.00f, 0.40f, 1.00f},
    {"Transparent",    {0.30f,0.30f,0.30f}, {0.80f,0.80f,0.80f}, {0.40f,0.40f,0.40f},  48.0f, 0.00f, 0.40f, 0.35f},
    {"BrushedMetal",   {0.28f,0.28f,0.30f}, {0.62f,0.62f,0.66f}, {0.85f,0.85f,0.88f},  24.0f, 1.00f, 0.55f, 1.00f},
    {"Diffuse",        {0.60f,0.60f,0.60f}, {0.95f,0.95f,0.95f}, {0.05f,0.05f,0.05f},   8.0f, 0.00f, 0.90f, 1.00f},
    {"Ghost",          {0.70f,0.70f,0.75f}, {0.85f,0.85f,0.90f}, {0.20f,0.20f,0.20f},  32.0f, 0.00f, 0.50f, 0.25f},
    {"Glass1",         {0.25f,0.25f,0.28f}, {0.70f,0.72f,0.75f}, {0.90f,0.90f,0.95f}, 128.0f, 0.00f, 0.05f, 0.30f},
    {"Glass2",         {0.22f,0.22f,0.26f}, {0.66f,0.68f,0.72f}, {0.95f,0.95f,1.00f}, 160.0f, 0.00f, 0.03f, 0.22f},
    {"Glass3",         {0.20f,0.20f,0.24f}, {0.62f,0.64f,0.70f}, {1.00f,1.00f,1.00f}, 200.0f, 0.00f, 0.02f, 0.15f},
    {"Glossy",         {0.30f,0.30f,0.30f}, {0.88f,0.88f,0.88f}, {0.90f,0.90f,0.90f}, 128.0f, 0.00f, 0.12f, 1.00f},
    {"HardPlastic",    {0.40f,0.40f,0.40f}, {0.86f,0.86f,0.86f}, {0.65f,0.65f,0.65f},  64.0f, 0.00f, 0.30f, 1.00f},
    {"MetallicPastel", {0.45f,0.45f,0.48f}, {0.78f,0.78f,0.82f}, {0.80f,0.80f,0.84f},  48.0f, 0.85f, 0.45f, 1.00f},
    {"Steel",          {0.25f,0.25f,0.27f}, {0.58f,0.58f,0.62f}, {0.95f,0.95f,0.98f}, 128.0f, 1.00f, 0.25f, 1.00f},
    {"Translucent",    {0.45f,0.45f,0.45f}, {0.82f,0.82f,0.82f}, {0.45f,0.45f,0.45f},  48.0f, 0.00f, 0.40f, 0.60f},
    {"Edgy",           {0.10f,0.10f,0.10f}, {0.80f,0.80f,0.80f}, {0.55f,0.55f,0.55f},  32.0f, 0.00f, 0.60f, 1.00f},
    {"EdgyShiny",      {0.10f,0.10f,0.10f}, {0.85f,0.85f,0.85f}, {0.95f,0.95f,0.95f}, 128.0f, 0.00f, 0.20f, 1.00f},
    {"EdgyGlass",      {0.10f,0.10f,0.12f}, {0.75f,0.78f,0.82f}, {1.00f,1.00f,1.00f}, 160.0f, 0.00f, 0.05f, 0.30f},
    {"Goodsell",       {0.75f,0.75f,0.75f}, {0.92f,0.92f,0.92f}, {0.10f,0.10f,0.10f},  16.0f, 0.00f, 0.80f, 1.00f},
    {"AOShiny",        {0.70f,0.70f,0.70f}, {0.90f,0.90f,0.90f}, {0.85f,0.85f,0.85f}, 128.0f, 0.00f, 0.20f, 1.00f},
    {"AOChalky",       {0.80f,0.80f,0.80f}, {0.94f,0.94f,0.94f}, {0.05f,0.05f,0.05f},   8.0f, 0.00f, 0.90f, 1.00f},
    {"AOEdgy",         {0.65f,0.65f,0.65f}, {0.88f,0.88f,0.88f}, {0.45f,0.45f,0.45f},  32.0f, 0.00f, 0.55f, 1.00f},
    {"BlownGlass",     {0.30f,0.32f,0.35f}, {0.72f,0.76f,0.80f}, {0.95f,0.95f,1.00f}, 140.0f, 0.00f, 0.04f, 0.28f},
    {"GlassBubble",    {0.35f,0.38f,0.42f}, {0.78f,0.82f,0.88f}, {1.00f,1.00f,1.00f}, 180.0f, 0.00f, 0.02f, 0.18f},
    {"RTChrome",       {0.20f,0.20f,0.22f}, {0.55f,0.55f,0.58f}, {1.00f,1.00f,1.00f}, 220.0f, 1.00f, 0.05f, 1.00f},
};
inline constexpr int kMaterialPresetCount =
    static_cast<int>(sizeof(kMaterialPresets) / sizeof(kMaterialPresets[0]));

// 越界返回第一项（默认 Opaque）—— 调用方无需自行判界
inline const MaterialPreset& materialPresetAt(int index) {
    return (index < 0 || index >= kMaterialPresetCount) ? kMaterialPresets[0]
                                                        : kMaterialPresets[index];
}

// 材质名 → 下标；未知名返回 -1（.mlscene 载入时用：名字是稳定键，下标不是）
inline int materialPresetIndexByName(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int i = 0; i < kMaterialPresetCount; ++i) {
        const char* a = kMaterialPresets[i].name;
        const char* b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return -1;
}

// 键渲染参数：计算键圆柱端点（保证"键不穿球、球不吞键"）
struct BondRenderParams {
    // 输入：两端原子中心与球半径、键圆柱半径
    // 输出：圆柱端点 s/e（沿键轴缩进后）
    static void computeEndpoints(const glm::vec3& posA, const glm::vec3& posB,
                                 float radiusA, float radiusB, float cylRadius,
                                 glm::vec3* outS, glm::vec3* outE) {
        const glm::vec3 axis = posB - posA;
        const float bondLen = glm::length(axis);
        if (bondLen < 1e-6f) {
            // 退化：重叠原子，0.49 兜底（占键长比例）
            *outS = posA;
            *outE = posB;
            return;
        }
        const glm::vec3 dir = axis / bondLen;

        // 端面缩进：d = sqrt(sphereR² - cylR²)（勾股定理，端面圆周与球面相切）
        auto inset = [&](float sphereR) -> float {
            if (sphereR <= cylRadius) {
                return 0.0f;
            }
            return std::sqrt(sphereR * sphereR - cylRadius * cylRadius);
        };
        float dA = inset(radiusA);
        float dB = inset(radiusB);

        // 防御 1：偏移量 ≤ 键长 40%（防隧穿）
        const float kMaxInset = bondLen * 0.4f;
        dA = std::min(dA, kMaxInset);
        dB = std::min(dB, kMaxInset);

        // 防御 2：极短键按比例收缩（两端合计 ≤ 0.98 键长）
        const float total = dA + dB;
        if (total > bondLen * 0.98f) {
            const float k = (bondLen * 0.98f) / total;
            dA *= k;
            dB *= k;
        }

        *outS = posA + dir * dA;
        *outE = posB - dir * dB;
    }

    // 便捷重载（球半径 = 0.25×vdW 语义由调用方算好传入）
    static void computeEndpoints(const glm::vec3& posA, const glm::vec3& posB,
                                 float radiusA, float radiusB,
                                 glm::vec3* outS, glm::vec3* outE) {
        computeEndpoints(posA, posB, radiusA, radiusB, 0.15f, outS, outE);
    }
};

} // namespace moleculens::engine
