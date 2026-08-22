// =============================================================================
// MolecuLens · engine · rep/representation.h —— 表示三元组（方法/配色/作用域）
// 规格来源：主方案 §5.2（类型/配色/选择字符串/半径缩放/可见性）
// =============================================================================

#pragma once

#include <string>

namespace moleculens::engine {

enum class RepresentationType : uint8_t {
    Wireframe, Stick, BallAndStick, Spacefill, Cartoon, Surface, Label,
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
    float            radiusScale = 1.0f;
    bool             visible = true;

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
