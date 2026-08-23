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

#include <glm/glm.hpp>

namespace moleculens::engine {

// 光照参数（全局光/全局光照方案，无方向光/点光源；主方案 §6.6.4 光照 → 全局光）。
// 全局 UBO 光照来源：RenderView 携带 → 各后端统一从 view 构建（而非各自硬编码）。
// 语义与着色器 GlobalUBO 匹配：skyColor → lightDir 槽（天顶色）、groundColor →
// lightColor 槽（地面色）、ambientColor+w → ambient 槽（基础色+半球光强度）。
struct LightParams {
    glm::vec3 skyColor{1.0f, 1.0f, 1.0f};         // 天顶色（半球上方）
    glm::vec3 groundColor{0.35f, 0.4f, 0.5f};     // 地面色（半球下方）
    glm::vec3 ambientColor{0.55f, 0.6f, 0.75f};   // 均匀环境光基础色
    float ambientIntensity = 0.45f;               // 半球光强度

    static LightParams defaults() { return {}; }
};

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
