// =============================================================================
// MolecuLens · engine · rep/representation.h —— 表示三元组（方法/配色/作用域）
// 规格来源：主方案 §5.2（类型/配色/选择字符串/半径缩放/可见性）
// =============================================================================

#pragma once

#include <string>

namespace moleculens::engine {

enum class RepresentationType : uint8_t {
    Wireframe,
    Stick,
    BallAndStick,
    Spacefill,
    Cartoon,
    Surface,
    Label,
    Vdw,         // 范德华半径空间填充（全 vdW 球，不受 radiusScale 缩放——区别于 CPK）
    QuickSurf,   // 高斯密度等值面（radscale=radiusScale, gridspacing, isovalue）
    Licorice,    // 甘草棍（粗棍状，均匀半径，无端帽球）
    Tube,        // 管状（backbone 平滑管，简化卡通）
    NewCartoon,  // 改进卡通（更平滑的样条，更好的二级结构表现）
    Ribbon,      // 丝带（薄带状，仅 backbone）
    Count,
};

enum class ColorScheme : uint8_t {
    ElementCPK, Chain, Residue, BFactor, Uniform,
    Count,
};

// 表示 = 三元组（方法 / 配色 / 作用域）正交（主方案 §5.2）
struct Representation {
    std::string      name;
    RepresentationType type = RepresentationType::BallAndStick;
    std::string      selection;            // Selection 字符串（§4.6），"" = all
    ColorScheme      colorScheme = ColorScheme::ElementCPK;
    float            radiusScale = 1.0f;   // 球体/原子半径整体缩放
    float            bondScale = 1.0f;     // 键圆柱半径缩放（§5.7.2 cylR = 0.15×radiusScale×bondScale）
    bool             visible = true;

    // 体系级表面：对整个体系（含溶剂分子）生成一个整体表面（G5）。
    // 默认 false = 每实例一个表面（小分子用）；true = 渲染器按体系聚合全部可见
    // 原子（含溶剂）为一个表面。适用于 Surface 与 QuickSurf 两种模式。
    bool systemSurface = false;

    // QuickSurf 参数（参考 other/QuickSurf：radscale 复用 radiusScale）
    float            surfIsovalue = 0.5f;   // 等值面阈值（密度）
    float            surfGridSpacing = 0.5f;  // 网格间距（Å）

    // 材质（MaterialBlock 参数，§6.6.2 预设；默认预设）
    float ambient[4]   = {0.60f, 0.60f, 0.60f, 1.0f};
    float diffuse[4]   = {0.90f, 0.90f, 0.90f, 1.0f};
    float specular[4]  = {0.30f, 0.30f, 0.30f, 1.0f};
    float shininess    = 32.0f;
    float metallic     = 0.0f;
    float roughness    = 0.40f;
    float opacity      = 1.0f;
};

const char* representationTypeName(RepresentationType);
const char* colorSchemeName(ColorScheme);

// 中文名（UI 用）
const char* representationTypeZh(RepresentationType);
const char* colorSchemeZh(ColorScheme);

} // namespace moleculens::engine
