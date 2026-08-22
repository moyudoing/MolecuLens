// =============================================================================
// MolecuLens · entity · element/element.h —— 元素与原子
// 规格来源：主方案 §4.1（ElementType / ElementInfo 静态表）
// 实现要求：ElementInfo 表以 constexpr std::array<ElementInfo, 119> 静态初始化
// =============================================================================

#pragma once

#include <array>
#include <cstdint>

namespace moleculens::entity {

enum class ElementType : uint8_t {
    Hydrogen = 1, Helium, Lithium, Beryllium, Boron, Carbon, Nitrogen, Oxygen,
    Fluorine, Neon, Sodium, Magnesium, Aluminum, Silicon, Phosphorus, Sulfur,
    Chlorine, Argon, Potassium, Calcium, Scandium, Titanium, Vanadium, Chromium,
    Manganese, Iron, Cobalt, Nickel, Copper, Zinc, Gallium, Germanium, Arsenic,
    Selenium, Bromine, Krypton, Rubidium, Strontium, Yttrium, Zirconium, Niobium,
    Molybdenum, Technetium, Ruthenium, Rhodium, Palladium, Silver, Cadmium,
    Indium, Tin, Antimony, Tellurium, Iodine, Xenon, Cesium, Barium, Lanthanum,
    Cerium, Praseodymium, Neodymium, Promethium, Samarium, Europium, Gadolinium,
    Terbium, Dysprosium, Holmium, Erbium, Thulium, Ytterbium, Lutetium, Hafnium,
    Tantalum, Tungsten, Rhenium, Osmium, Iridium, Platinum, Gold, Mercury,
    Thallium, Lead, Bismuth, Polonium, Astatine, Radon, Francium, Radium,
    Actinium, Thorium, Protactinium, Uranium, Neptunium, Plutonium, Americium,
    Curium, Berkelium, Californium, Einsteinium, Fermium, Mendelevium, Nobelium,
    Lawrencium, Rutherfordium, Dubnium, Seaborgium, Bohrium, Hassium, Meitnerium,
    Darmstadtium, Roentgenium, Copernicium, Nihonium, Flerovium, Moscovium,
    Livermorium, Tennessine, Oganesson = 118,
    Unknown = 0xFF,
};

struct ElementInfo {                     // 静态表，下标 = ElementType
    const char* symbol;                  // "H"
    const char* name;                    // "Hydrogen"
    uint8_t     atomicNumber;
    float       covalentRadius;          // Å
    float       vdwRadius;               // Å
    float       mass;                    // u
    float       color[3];                // CPK 色（sRGB）
};

// 元素符号 → 类型（"C" → Carbon；未知 → Unknown）
ElementType elementFromSymbol(const char* symbol) noexcept;

// 类型 → 信息；越界/Unknown 返回 Unknown 占位项
constexpr const ElementInfo& elementInfo(ElementType) noexcept;

// 表内容见 element.cpp（constexpr 完整定义，下标 0 为 Unknown 占位）
extern const std::array<ElementInfo, 119> kElementTable;

inline constexpr const ElementInfo& elementInfo(ElementType type) noexcept {
    const uint8_t idx = static_cast<uint8_t>(type);
    return (idx >= 1 && idx < kElementTable.size())
               ? kElementTable[idx]
               : kElementTable[0];
}

} // namespace moleculens::entity
