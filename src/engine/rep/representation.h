// =============================================================================
// MolecuLens · engine · rep/representation.h —— 表示三元组（方法/配色/作用域）
// 规格来源：主方案 §5.2（类型/配色/选择字符串/半径缩放/可见性）
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace moleculens::engine {

// 表示类型（数值即 style_registry 静态分派表下标）。
// 注：v1 曾在 Surface 之后预留 Label（标签）槽位，M2 未实现，已移除并让后续
//     类型前移（Vdw=6 … EspSurface=12, Count=13）；命令协议中 Label 命令的
//     枚举槽位（值 4）保留（见 cmd.h，旧 .mlscene 命令流兼容），不再发射/消费。
enum class RepresentationType : uint8_t {
    Wireframe,
    Stick,
    BallAndStick,
    Spacefill,
    Cartoon,
    Surface,
    Vdw,         // 范德华半径空间填充（全 vdW 球，不受 radiusScale 缩放——区别于 CPK）
    QuickSurf,   // 高斯密度等值面（radscale=radiusScale, gridspacing, isovalue）
    Licorice,    // 甘草棍（粗棍状，均匀半径，无端帽球）
    Tube,        // 管状（backbone 平滑管，简化卡通）
    NewCartoon,  // 改进卡通（更平滑的样条，更好的二级结构表现）
    Ribbon,      // 丝带（薄带状，仅 backbone）
    EspSurface,  // ESP 映射表面（电子密度等值面 + 静电势逐顶点着色；输入 cube 文件）
    Plate,       // 平板：每个残基/分子一块扁平方板（辅基/配体示意图风格）
    Diamond,     // 菱形：每个非氢原子一颗八面体宝石（示意图 / 粗粒化观感）
    DotCloud,    // 点云：每个原子一个近似的点精灵（超大体系快速预览，无键）
    Polyhedron,  // 多面体：每个非氢原子一颗二十面体（平面法线、随机朝向）
    Count,
};

enum class ColorScheme : uint8_t {
    ElementCPK, Chain, Residue, BFactor, Uniform,
    // VMD「Coloring Method = Name」的观感调色板（v2.3）：
    // C 青、N 蓝、O 红、H 白、S 黄、P 橙，其余回落 CPK。
    // 与 ElementCPK 的区别就是碳——CPK 的碳是灰色，VMD Name 的碳是青色，
    // 这正是用户对比 VMD 截图时觉得"颜色不一样"的地方。
    AtomName,
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

    // —— ESP 映射表面参数（type == EspSurface 时生效）——
    // 数据来源：Gaussian / Multiwfn 导出的 cube 文件。density 定等值面几何，
    // ESP 定逐顶点颜色（见 engine/style/espsurface/esp_surface.h）。
    std::string      espDensityCubePath;     // 电子密度 cube（几何来源）
    std::string      espPotentialCubePath;   // 静电势 cube（颜色来源）
    float            espIsovalue = 0.001f;   // 密度等值面阈值（a.u.，0.001 ≈ vdW 表面）
    float            espVminKcal = -45.0f;   // 色标下限（kcal/mol；autoRange 时忽略）
    float            espVmaxKcal = 45.0f;    // 色标上限（kcal/mol）
    bool             espAutoRange = true;    // 按 ESP 数据自动定色标范围
    bool             espInvert = false;      // 翻转色标方向（低值端 ↔ 高值端）
    int32_t          espColormap = 0;        // 色标方案（EspColormap 枚举值，0=RWB）

    // 选中的材质预设下标（见 render_parameter.h 的 kMaterialPresets，共 23 项）。
    // 作用只是让 UI 下拉**精确回显**用户选的是哪一项 —— 材质数值本身存在下面 7 个
    // 字段里。手动改任一参数即置 -1（"自定义"），此时下拉显示"自定义"。
    // 若只按数值容差反查预设，Glass1/2/3 这类数值接近的材质会互相误判。
    int32_t materialPreset = 0;

    // 材质（MaterialBlock 参数，§6.6.2 预设；默认预设）
    float ambient[4]   = {0.60f, 0.60f, 0.60f, 1.0f};
    float diffuse[4]   = {0.90f, 0.90f, 0.90f, 1.0f};
    float specular[4]  = {0.30f, 0.30f, 0.30f, 1.0f};
    float shininess    = 32.0f;
    float metallic     = 0.0f;
    float roughness    = 0.40f;
    float opacity      = 1.0f;
};

const char* representationTypeName(RepresentationType) noexcept;
const char* colorSchemeName(ColorScheme) noexcept;

// 中文名（UI 用）
const char* representationTypeZh(RepresentationType) noexcept;
const char* colorSchemeZh(ColorScheme) noexcept;

} // namespace moleculens::engine
