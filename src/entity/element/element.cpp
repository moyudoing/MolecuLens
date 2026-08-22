// =============================================================================
// MolecuLens · entity · element/element.cpp —— ElementInfo 静态表（118 元素）
// 规格来源：主方案 §4.1（constexpr std::array<ElementInfo, 119>；
//           单元测试校验 118 个元素完整、颜色为合法 sRGB）
// 数据：共价半径（Cordero 等）/ vdW 半径（Bondi+扩展）/ 质量 / CPK 色（sRGB）
// 行格式：{ "H", "Hydrogen", 1, cov, vdw, mass, {r,g,b} }
// =============================================================================

#include "entity/element/element.h"

#include <string_view>

namespace moleculens::entity {

namespace {

// 紧凑行宏：S=符号, N=名称, Z=原子序数, CV=共价半径(Å), VD=vdW(Å), M=质量(u), R,G,B=CPK 色
#define EL(S, N, Z, CV, VD, M, R, G, B) \
    { S, N, Z, CV, VD, M, { R, G, B } }

constexpr ElementInfo kTable[] = {
    // 下标 0：Unknown 占位（elementInfo 越界兜底）
    EL("Xx", "Unknown", 0, 0.0f, 0.0f, 0.0f, 0.78f, 0.78f, 0.78f),
    EL("H",  "Hydrogen",     1, 0.31f, 1.20f,  1.008f, 1.00f, 1.00f, 1.00f),
    EL("He", "Helium",       2, 0.28f, 1.40f,  4.003f, 0.85f, 1.00f, 1.00f),
    EL("Li", "Lithium",      3, 1.28f, 1.82f,  6.94f,  0.80f, 0.50f, 1.00f),
    EL("Be", "Beryllium",    4, 0.96f, 1.53f,  9.012f, 0.76f, 1.00f, 0.00f),
    EL("B",  "Boron",        5, 0.84f, 1.92f, 10.81f,  1.00f, 0.71f, 0.71f),
    EL("C",  "Carbon",       6, 0.76f, 1.70f, 12.011f, 0.56f, 0.56f, 0.56f),
    EL("N",  "Nitrogen",     7, 0.71f, 1.55f, 14.007f, 0.19f, 0.31f, 0.97f),
    EL("O",  "Oxygen",       8, 0.66f, 1.52f, 15.999f, 1.00f, 0.05f, 0.05f),
    EL("F",  "Fluorine",     9, 0.57f, 1.47f, 18.998f, 0.56f, 0.88f, 0.31f),
    EL("Ne", "Neon",        10, 0.58f, 1.54f, 20.180f, 0.70f, 0.89f, 0.96f),
    EL("Na", "Sodium",      11, 1.66f, 2.27f, 22.990f, 0.67f, 0.36f, 0.95f),
    EL("Mg", "Magnesium",   12, 1.41f, 1.73f, 24.305f, 0.54f, 1.00f, 0.00f),
    EL("Al", "Aluminum",    13, 1.21f, 1.84f, 26.982f, 0.75f, 0.65f, 0.65f),
    EL("Si", "Silicon",     14, 1.11f, 2.10f, 28.085f, 0.94f, 0.78f, 0.63f),
    EL("P",  "Phosphorus",  15, 1.07f, 1.80f, 30.974f, 1.00f, 0.50f, 0.00f),
    EL("S",  "Sulfur",      16, 1.05f, 1.80f, 32.06f,  1.00f, 1.00f, 0.19f),
    EL("Cl", "Chlorine",    17, 1.02f, 1.75f, 35.45f,  0.12f, 0.94f, 0.12f),
    EL("Ar", "Argon",       18, 1.06f, 1.88f, 39.948f, 0.50f, 0.82f, 0.89f),
    EL("K",  "Potassium",   19, 2.03f, 2.75f, 39.098f, 0.56f, 0.25f, 0.83f),
    EL("Ca", "Calcium",     20, 1.76f, 2.31f, 40.078f, 0.24f, 1.00f, 0.00f),
    EL("Sc", "Scandium",    21, 1.70f, 2.11f, 44.956f, 0.90f, 0.90f, 0.90f),
    EL("Ti", "Titanium",    22, 1.60f, 2.00f, 47.867f, 0.75f, 0.76f, 0.78f),
    EL("V",  "Vanadium",    23, 1.53f, 2.05f, 50.942f, 0.65f, 0.65f, 0.67f),
    EL("Cr", "Chromium",    24, 1.39f, 2.05f, 51.996f, 0.54f, 0.60f, 0.78f),
    EL("Mn", "Manganese",   25, 1.39f, 2.05f, 54.938f, 0.61f, 0.48f, 0.78f),
    EL("Fe", "Iron",        26, 1.32f, 2.05f, 55.845f, 0.88f, 0.40f, 0.20f),
    EL("Co", "Cobalt",      27, 1.26f, 2.00f, 58.933f, 0.94f, 0.56f, 0.63f),
    EL("Ni", "Nickel",      28, 1.24f, 2.00f, 58.693f, 0.31f, 0.82f, 0.31f),
    EL("Cu", "Copper",      29, 1.32f, 1.96f, 63.546f, 0.78f, 0.50f, 0.20f),
    EL("Zn", "Zinc",        30, 1.22f, 2.01f, 65.38f,  0.49f, 0.50f, 0.69f),
    EL("Ga", "Gallium",     31, 1.22f, 1.87f, 69.723f, 0.76f, 0.56f, 0.56f),
    EL("Ge", "Germanium",   32, 1.20f, 2.11f, 72.630f, 0.40f, 0.56f, 0.56f),
    EL("As", "Arsenic",     33, 1.19f, 1.85f, 74.922f, 0.74f, 0.50f, 0.89f),
    EL("Se", "Selenium",    34, 1.20f, 1.90f, 78.971f, 1.00f, 0.63f, 0.00f),
    EL("Br", "Bromine",     35, 1.20f, 1.85f, 79.904f, 0.65f, 0.16f, 0.16f),
    EL("Kr", "Krypton",     36, 1.16f, 2.02f, 83.798f, 0.36f, 0.72f, 0.82f),
    EL("Rb", "Rubidium",    37, 2.20f, 3.03f, 85.468f, 0.44f, 0.18f, 0.69f),
    EL("Sr", "Strontium",   38, 1.95f, 2.49f, 87.62f,  0.00f, 1.00f, 0.00f),
    EL("Y",  "Yttrium",     39, 1.90f, 2.32f, 88.906f, 0.58f, 1.00f, 1.00f),
    EL("Zr", "Zirconium",   40, 1.75f, 2.23f, 91.224f, 0.58f, 0.88f, 0.88f),
    EL("Nb", "Niobium",     41, 1.64f, 2.18f, 92.906f, 0.45f, 0.76f, 0.79f),
    EL("Mo", "Molybdenum",  42, 1.54f, 2.17f, 95.95f,  0.33f, 0.71f, 0.71f),
    EL("Tc", "Technetium",  43, 1.47f, 2.16f, 98.0f,   0.23f, 0.62f, 0.62f),
    EL("Ru", "Ruthenium",   44, 1.46f, 2.13f, 101.07f, 0.14f, 0.56f, 0.56f),
    EL("Rh", "Rhodium",     45, 1.42f, 2.10f, 102.906f,0.04f, 0.49f, 0.55f),
    EL("Pd", "Palladium",   46, 1.39f, 2.10f, 106.42f, 0.00f, 0.41f, 0.52f),
    EL("Ag", "Silver",      47, 1.45f, 2.11f, 107.868f,0.75f, 0.75f, 0.75f),
    EL("Cd", "Cadmium",     48, 1.44f, 2.18f, 112.414f,1.00f, 0.85f, 0.56f),
    EL("In", "Indium",      49, 1.42f, 1.93f, 114.818f,0.65f, 0.46f, 0.45f),
    EL("Sn", "Tin",         50, 1.39f, 2.17f, 118.710f,0.40f, 0.50f, 0.50f),
    EL("Sb", "Antimony",    51, 1.39f, 2.06f, 121.760f,0.62f, 0.39f, 0.71f),
    EL("Te", "Tellurium",   52, 1.38f, 2.06f, 127.60f, 0.83f, 0.48f, 0.00f),
    EL("I",  "Iodine",      53, 1.39f, 1.98f, 126.904f,0.58f, 0.00f, 0.58f),
    EL("Xe", "Xenon",       54, 1.40f, 2.16f, 131.293f,0.26f, 0.62f, 0.69f),
    EL("Cs", "Cesium",      55, 2.44f, 3.43f, 132.905f,0.34f, 0.09f, 0.56f),
    EL("Ba", "Barium",      56, 2.15f, 2.68f, 137.327f,0.00f, 0.79f, 0.00f),
    EL("La", "Lanthanum",   57, 2.07f, 2.43f, 138.905f,0.44f, 0.83f, 1.00f),
    EL("Ce", "Cerium",      58, 2.04f, 2.42f, 140.116f,1.00f, 1.00f, 0.78f),
    EL("Pr", "Praseodymium",59, 2.03f, 2.40f, 140.908f,0.85f, 1.00f, 0.78f),
    EL("Nd", "Neodymium",   60, 2.01f, 2.39f, 144.242f,0.78f, 1.00f, 0.78f),
    EL("Pm", "Promethium",  61, 1.99f, 2.38f, 145.0f,  0.64f, 1.00f, 0.78f),
    EL("Sm", "Samarium",    62, 1.98f, 2.36f, 150.36f, 0.56f, 1.00f, 0.78f),
    EL("Eu", "Europium",    63, 1.98f, 2.35f, 151.964f,0.38f, 1.00f, 0.78f),
    EL("Gd", "Gadolinium",  64, 1.96f, 2.34f, 157.25f, 0.27f, 1.00f, 0.78f),
    EL("Tb", "Terbium",     65, 1.94f, 2.33f, 158.925f,0.19f, 1.00f, 0.78f),
    EL("Dy", "Dysprosium",  66, 1.92f, 2.32f, 162.500f,0.12f, 1.00f, 0.78f),
    EL("Ho", "Holmium",     67, 1.92f, 2.31f, 164.930f,0.00f, 1.00f, 0.61f),
    EL("Er", "Erbium",      68, 1.89f, 2.30f, 167.259f,0.00f, 0.90f, 0.46f),
    EL("Tm", "Thulium",     69, 1.90f, 2.29f, 168.934f,0.00f, 0.83f, 0.32f),
    EL("Yb", "Ytterbium",   70, 1.87f, 2.28f, 173.045f,0.00f, 0.75f, 0.22f),
    EL("Lu", "Lutetium",    71, 1.87f, 2.27f, 174.967f,0.00f, 0.67f, 0.14f),
    EL("Hf", "Hafnium",     72, 1.75f, 2.23f, 178.49f, 0.30f, 0.76f, 1.00f),
    EL("Ta", "Tantalum",    73, 1.70f, 2.22f, 180.948f,0.30f, 0.65f, 1.00f),
    EL("W",  "Tungsten",    74, 1.62f, 2.18f, 183.84f, 0.13f, 0.58f, 0.84f),
    EL("Re", "Rhenium",     75, 1.51f, 2.16f, 186.207f,0.15f, 0.49f, 0.67f),
    EL("Os", "Osmium",      76, 1.44f, 2.16f, 190.23f, 0.15f, 0.40f, 0.59f),
    EL("Ir", "Iridium",     77, 1.41f, 2.08f, 192.217f,0.09f, 0.33f, 0.53f),
    EL("Pt", "Platinum",    78, 1.36f, 2.09f, 195.084f,0.82f, 0.82f, 0.88f),
    EL("Au", "Gold",        79, 1.36f, 2.17f, 196.967f,1.00f, 0.82f, 0.14f),
    EL("Hg", "Mercury",     80, 1.32f, 2.09f, 200.592f,0.72f, 0.72f, 0.82f),
    EL("Tl", "Thallium",    81, 1.45f, 2.09f, 204.38f, 0.65f, 0.33f, 0.30f),
    EL("Pb", "Lead",        82, 1.46f, 2.02f, 207.2f,  0.34f, 0.35f, 0.38f),
    EL("Bi", "Bismuth",     83, 1.48f, 2.07f, 208.980f,0.62f, 0.31f, 0.71f),
    EL("Po", "Polonium",    84, 1.40f, 1.97f, 209.0f,  0.67f, 0.36f, 0.00f),
    EL("At", "Astatine",    85, 1.50f, 2.02f, 210.0f,  0.46f, 0.31f, 0.27f),
    EL("Rn", "Radon",       86, 1.50f, 2.20f, 222.0f,  0.26f, 0.51f, 0.59f),
    EL("Fr", "Francium",    87, 2.60f, 3.48f, 223.0f,  0.26f, 0.00f, 0.40f),
    EL("Ra", "Radium",      88, 2.21f, 2.83f, 226.0f,  0.00f, 0.49f, 0.00f),
    EL("Ac", "Actinium",    89, 2.15f, 2.79f, 227.0f,  0.44f, 0.67f, 0.98f),
    EL("Th", "Thorium",     90, 2.06f, 2.74f, 232.038f,0.00f, 0.73f, 1.00f),
    EL("Pa", "Protactinium",91, 2.00f, 2.70f, 231.036f,0.00f, 0.63f, 1.00f),
    EL("U",  "Uranium",     92, 1.96f, 2.67f, 238.029f,0.00f, 0.56f, 1.00f),
    EL("Np", "Neptunium",   93, 1.90f, 2.64f, 237.0f,  0.00f, 0.50f, 1.00f),
    EL("Pu", "Plutonium",   94, 1.87f, 2.62f, 244.0f,  0.00f, 0.42f, 1.00f),
    EL("Am", "Americium",   95, 1.80f, 2.59f, 243.0f,  0.33f, 0.36f, 0.95f),
    EL("Cm", "Curium",      96, 1.69f, 2.57f, 247.0f,  0.47f, 0.36f, 0.89f),
    EL("Bk", "Berkelium",   97, 1.68f, 2.55f, 247.0f,  0.54f, 0.31f, 0.89f),
    EL("Cf", "Californium", 98, 1.68f, 2.55f, 251.0f,  0.63f, 0.21f, 0.83f),
    EL("Es", "Einsteinium", 99, 1.65f, 2.53f, 252.0f,  0.70f, 0.12f, 0.83f),
    EL("Fm", "Fermium",    100, 1.67f, 2.52f, 257.0f,  0.70f, 0.12f, 0.73f),
    EL("Md", "Mendelevium",101, 1.73f, 2.51f, 258.0f,  0.70f, 0.05f, 0.65f),
    EL("No", "Nobelium",   102, 1.76f, 2.50f, 259.0f,  0.74f, 0.05f, 0.53f),
    EL("Lr", "Lawrencium", 103, 1.61f, 2.49f, 266.0f,  0.78f, 0.00f, 0.40f),
    EL("Rf", "Rutherfordium",104,1.57f, 2.48f, 267.0f,  0.80f, 0.00f, 0.35f),
    EL("Db", "Dubnium",    105, 1.49f, 2.47f, 268.0f,  0.82f, 0.00f, 0.31f),
    EL("Sg", "Seaborgium", 106, 1.43f, 2.46f, 269.0f,  0.85f, 0.00f, 0.27f),
    EL("Bh", "Bohrium",    107, 1.41f, 2.45f, 270.0f,  0.88f, 0.00f, 0.22f),
    EL("Hs", "Hassium",    108, 1.34f, 2.44f, 277.0f,  0.90f, 0.00f, 0.18f),
    EL("Mt", "Meitnerium", 109, 1.29f, 2.43f, 278.0f,  0.92f, 0.00f, 0.15f),
    EL("Ds", "Darmstadtium",110,1.28f, 2.42f, 281.0f,  0.96f, 0.00f, 0.13f),
    EL("Rg", "Roentgenium",111, 1.21f, 2.41f, 282.0f,  1.00f, 0.00f, 0.12f),
    EL("Cn", "Copernicium",112, 1.22f, 2.40f, 285.0f,  1.00f, 0.00f, 0.00f),
    EL("Nh", "Nihonium",   113, 1.36f, 2.39f, 286.0f,  0.96f, 0.75f, 0.75f),
    EL("Fl", "Flerovium",  114, 1.43f, 2.38f, 289.0f,  0.90f, 0.75f, 0.75f),
    EL("Mc", "Moscovium",  115, 1.62f, 2.37f, 290.0f,  0.85f, 0.75f, 0.75f),
    EL("Lv", "Livermorium",116, 1.75f, 2.36f, 293.0f,  0.80f, 0.75f, 0.75f),
    EL("Ts", "Tennessine", 117, 1.65f, 2.35f, 294.0f,  0.75f, 0.75f, 0.75f),
    EL("Og", "Oganesson",  118, 1.57f, 2.34f, 294.0f,  0.70f, 0.70f, 0.70f),
};

#undef EL

} // namespace

const std::array<ElementInfo, 119> kElementTable = [] {
    std::array<ElementInfo, 119> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        table[i] = kTable[i];
    }
    return table;
}();

ElementType elementFromSymbol(const char* symbol) noexcept {
    if (symbol == nullptr || symbol[0] == '\0') {
        return ElementType::Unknown;
    }
    using namespace std::string_view_literals;
    static constexpr std::string_view syms[119] = {
        "", "H", "He", "Li", "Be", "B", "C", "N", "O", "F", "Ne",
        "Na", "Mg", "Al", "Si", "P", "S", "Cl", "Ar", "K", "Ca",
        "Sc", "Ti", "V", "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn",
        "Ga", "Ge", "As", "Se", "Br", "Kr", "Rb", "Sr", "Y", "Zr",
        "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd", "In", "Sn",
        "Sb", "Te", "I", "Xe", "Cs", "Ba", "La", "Ce", "Pr", "Nd",
        "Pm", "Sm", "Eu", "Gd", "Tb", "Dy", "Ho", "Er", "Tm", "Yb",
        "Lu", "Hf", "Ta", "W", "Re", "Os", "Ir", "Pt", "Au", "Hg",
        "Tl", "Pb", "Bi", "Po", "At", "Rn", "Fr", "Ra", "Ac", "Th",
        "Pa", "U", "Np", "Pu", "Am", "Cm", "Bk", "Cf", "Es", "Fm",
        "Md", "No", "Lr", "Rf", "Db", "Sg", "Bh", "Hs", "Mt", "Ds",
        "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og",
    };
    std::string_view sv(symbol);
    for (uint8_t i = 1; i < 119; ++i) {
        if (sv == syms[i]) {
            return static_cast<ElementType>(i);
        }
    }
    return ElementType::Unknown;
}

} // namespace moleculens::entity
