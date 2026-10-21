// =============================================================================
// MolecuLens · engine · style/style_registry.h —— 渲染样式注册表（注册/分发层）
// 职责：提供两种注册时机的 API——①编译期/静态注册（样式随二进制一次性声明，
//       静态分派表）；②运行时/动态注册（编译完成后由外部模块——动态库/插件——
//       注入或替换样式）。
// 分发与代表层收敛：本头构建以 RepresentationType 为键的分派表，是样式分派的
// 唯一事实来源（Representer::generate 经 styleFor() 取入口，不再自建分派表）；
// 引擎内部热路径直接索引编译期静态表（无锁），仅在插件注册后才走运行时段。
// 规格来源：主方案 §5.7 各类渲染样式 / §5.2（表示·样式分派）
// 关联：base_style.h（统一函数原型 StyleDrawFn）
// =============================================================================

#ifndef STYLE_REGISTRY_H
#define STYLE_REGISTRY_H

#include <array>
#include <cstddef>
#include <mutex>

#include "engine/style/base_style.h"

namespace moleculens::engine {

// =============================================================================
// ① 编译期 / 静态注册（compile-time static registration）
// =============================================================================
// 适用场景：样式集合在编译期固定、随可执行文件/库一次性链接；无外部扩展需求。
// 机制：静态分派表，index == static_cast<int>(RepresentationType)，用初始化列表
//       把每个枚举值映射到 base_style.h 声明的 draw* 入口（编译期即可完成）。
// 全 const 只读 → 无锁，供热路径（Representer::generate 每 rep×instance×frame）
// 直接索引，与既有 kDrawTable 语义等价。

inline constexpr StyleDrawFn kStyleStaticTable[static_cast<int>(RepresentationType::Count)] = {
    drawWireframe,         // 0  Wireframe    —— 线状（超大体系快速预览）
    drawStick,             // 1  Stick        —— 棍状
    drawBallAndStick,      // 2  BallAndStick —— 球棍（CPK）
    drawSpacefill,         // 3  Spacefill    —— 空间填充（VdW）
    drawCartoon,           // 4  Cartoon      —— 卡通（6 步蛋白质风格）
    drawSurface,           // 5  Surface      —— vdW/MC 等值面
    drawVdw,               // 6  Vdw          —— 范德华全半径空间填充
    drawQuickSurf,         // 7  QuickSurf    —— 高斯密度等值面
    drawLicorice,          // 8  Licorice     —— 甘草棍（粗圆柱，无端帽球）
    drawTube,              // 9  Tube         —— 管状（backbone 平滑管）
    drawNewCartoon,        // 10 NewCartoon   —— 改进卡通（更平滑样条）
    drawRibbon,            // 11 Ribbon       —— 丝带（薄带状，仅 backbone）
    drawEspSurface,        // 12 EspSurface   —— ESP 映射表面（密度等值面 + 电势着色）
    drawPlate,             // 13 Plate        —— 平板（每残基/分子一块扁平方板）
    drawDiamond,           // 14 Diamond      —— 菱形（每残基/分子一颗八面体宝石）
    drawDotCloud,          // 15 DotCloud     —— 点云（每原子一个点精灵）
};

// 编译期按类型查询：直接取静态表对应槽位；未注册/越界返回 nullptr（不改动表）。
// 注：scoped enum 的 < 比较为 C++20 特性，本工程是 C++17，故用整数下标越界判断。
constexpr inline StyleDrawFn staticStyleFor(RepresentationType type) noexcept {
    const auto i = static_cast<int>(type);
    return (i < 0 || i >= static_cast<int>(RepresentationType::Count))
               ? nullptr
               : kStyleStaticTable[i];
}

// =============================================================================
// ③ 结构类型 × 样式适用性（结构类型感知适配 — 单一事实来源）
// =============================================================================
// 动机：不同分子结构（蛋白质 vs 小分子/溶剂）有不同渲染需求。骨架样式（卡通/
//       丝带/管状/新卡通）仅对蛋白质（带残基/链的 backbone）有意义；小分子/溶剂
//       用线/棍/球棍/空间填充/表面。本组接口把「结构类型 → 适用样式子集 → 默认
//       样式」收敛到注册表，styles_tool 的 UI 列表与 RepresentationController 的
//       自动适配都从这里取，消除各处手写 styleNames[]/styleTypes[] 双数组。

// 结构类型分类（由数据层按残基/链判定后传入）
enum class StyleKind : uint8_t {
    General = 0,   // 小分子 / 溶剂 / 通用：任何结构都能显示的通用样式
    Protein,       // 蛋白质 / 多肽：具备 backbone 骨架样式（卡通等）
    Count,
};

// 权威中文显示名（原 styles_tool 手写 styleNames[] 的唯一事实来源；UI 组合框用）
const char* styleDisplayName(RepresentationType type) noexcept;

// 结构类型适用性：kind 下该样式是否可用
//   General → 全部可用；Protein → 仅骨架专属样式（Cartoon/NewCartoon/Ribbon/Tube）
bool styleSupportsKind(StyleKind kind, RepresentationType type) noexcept;

// 按结构类型推荐默认样式（蛋白质 → Cartoon；通用 → BallAndStick）。
// 供 StylesTool 在「当前表示不适用新结构类型」或首次适配时自动切换。
RepresentationType defaultStyle(StyleKind kind) noexcept;

// =============================================================================
// ② 运行时 / 动态注册（runtime dynamic registration）
// =============================================================================
// 适用场景：编译完成后，外部模块（动态库 .dll/.so，dlopen/dlsym，或插件系统）
//       注入新样式 / 替换既有样式，无需重新编译主程序。
// 机制：与静态表等宽的运行时覆盖层（runtime overlay）。register* 写入覆盖层槽位，
//       styleFor 查询时"先查运行时段，命中即返回；未命中回退编译期静态表"。
//       unregister 清除覆盖层 → 回退到静态表对应实现。
// 线程安全：覆盖层写读受全局互斥锁保护；热路径若需避免每帧加锁，可先用
//       staticStyleFor 直接取静态表（仅命中运行时段才走 styleFor 即可）。

// 运行时注册：把 type 对应样式的渲染入口替换/注入为 fn。
//   - 允许覆盖既有 type（含编译期已注册项）——覆盖后 styleFor 返回新 fn；
//   - fn 为 nullptr 时拒绝注册（返回 false）；
//   - 成功返回 true；失败（fn 为空）返回 false。
bool registerStyle(RepresentationType type, StyleDrawFn fn) noexcept;

// 运行时注销：清除 type 的运行时覆盖 → 回退编译期静态表对应实现。
//   - type 合法（非 Count/越界）时成功返回 true；否则 false。
bool unregisterStyle(RepresentationType type) noexcept;

// 运行时查询（含回退）：先查运行时覆盖层，命中即返回；未命中回退静态表。
//   - type 越界/未注册 → nullptr。
StyleDrawFn styleFor(RepresentationType type) noexcept;

// 运行时查询（仅运行时段，不回退）：取运行时覆盖，未覆盖 nullptr。
StyleDrawFn runtimeStyleFor(RepresentationType type) noexcept;

// —— 实现声明（style_registry.cpp）—— 本头仅接口。

} // namespace moleculens::engine

#endif // STYLE_REGISTRY_H