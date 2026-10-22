// =============================================================================
// MolecuLens · engine · style/style_registry.cpp —— 渲染样式运行时注册实现
// 职责：实现 style_registry.h 中声明的运行时（动态）注册 API：
//       registerStyle / unregisterStyle / styleFor / runtimeStyleFor。
// 机制：进程内与静态表等宽的运行时覆盖层（runtime overlay），写读受全局
//       互斥锁保护；styleFor "先查覆盖层，命中即返回，未命中回退静态表"。
// 规格来源：主方案 §5.7 各类渲染样式 / 插件（动态库）扩展机制
// 关联：base_style.h（StyleDrawFn）、style_registry.h（接口与静态表声明）
// =============================================================================

#include "engine/style/style_registry.h"

namespace moleculens::engine {
namespace {

// 与 kStyleStaticTable 等宽的运行时覆盖层：每个槽位独立记录是否被运行期覆盖。
// 索引 = static_cast<int>(RepresentationType)；Count 为哨兵、不存实现。
constexpr int kStyleCount = static_cast<int>(RepresentationType::Count);

struct RuntimeOverlay {
    std::array<StyleDrawFn, kStyleCount> fn{};
    std::array<bool,    kStyleCount>     active{};
};
RuntimeOverlay& overlay() {
    static RuntimeOverlay inst;   // 进程内单例（Meyers，线程安全惰性初始化）
    return inst;
}
std::mutex& overlayMutex() {
    static std::mutex mtx;
    return mtx;
}

bool typeIsCacheable(RepresentationType t) noexcept {
    const auto i = static_cast<int>(t);
    return t != RepresentationType::Count              // 哨兵不注册
        && i >= 0 && i < kStyleCount;
}

} // namespace

bool registerStyle(RepresentationType type, StyleDrawFn fn) noexcept {
    if (!typeIsCacheable(type) || fn == nullptr) {
        return false;                                  // 拒绝：type 非法或 fn 为空
    }
    std::lock_guard<std::mutex> lock(overlayMutex());
    const auto i = static_cast<int>(type);
    overlay().fn[i] = fn;
    overlay().active[i] = true;
    return true;
}

bool unregisterStyle(RepresentationType type) noexcept {
    if (!typeIsCacheable(type)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(overlayMutex());
    const auto i = static_cast<int>(type);
    overlay().fn[i] = nullptr;
    overlay().active[i] = false;                       // 清除覆盖 → 回退静态表
    return true;
}

StyleDrawFn runtimeStyleFor(RepresentationType type) noexcept {
    if (!typeIsCacheable(type)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(overlayMutex());
    const auto i = static_cast<int>(type);
    return overlay().active[i] ? overlay().fn[i] : nullptr;
}

StyleDrawFn styleFor(RepresentationType type) noexcept {
    // 先查运行时覆盖层，命中即返回；未命中回退编译期静态表。
    if (StyleDrawFn runtime = runtimeStyleFor(type); runtime != nullptr) {
        return runtime;
    }
    return staticStyleFor(type);                       // 编译期静态表（无锁）
}

// —— ③ 结构类型 × 样式适用性（style_registry.h 声明；单一事实来源）——

namespace {

// 权威中文显示名（按 RepresentationType 枚举顺序，与 representation.h 一一对应）
constexpr const char* kStyleNames[static_cast<int>(RepresentationType::Count)] = {
    "线状",      // 0  Wireframe    —— 键画线段
    "棍状",      // 1  Stick        —— 均匀半径棍
    "球棍",      // 2  BallAndStick —— 球 + 棍（CPK 语义）
    "空间填充",  // 3  Spacefill    —— 范德华半径空间填充
    "卡通",      // 4  Cartoon      —— 蛋白质骨架 6 步风格
    "表面",      // 5  Surface      —— vdW/MC 等值面
    "VDW",       // 6  Vdw          —— 范德华全半径空间填充（不受缩放）
    "QuickSurf", // 7  QuickSurf    —— 高斯密度等值面
    "甘草棍",    // 8  Licorice     —— 粗圆柱，无端帽球
    "管状",      // 9  Tube         —— backbone 平滑管
    "新卡通",    // 10 NewCartoon   —— 改进卡通（更平滑样条）
    "丝带",      // 11 Ribbon       —— 薄带状，仅 backbone
    "ESP 表面",  // 12 EspSurface   —— 密度等值面 + 静电势逐顶点着色
    "平板",      // 13 Plate        —— 每个残基/分子一块扁平方板（辅基示意）
    "菱形",      // 14 Diamond      —— 每个非氢原子一颗八面体宝石
    "点云",      // 15 DotCloud     —— 每个原子一个点精灵（超大体系）
    "多面体",    // 16 Polyhedron   —— 每个非氢原子一颗二十面体
};

// 结构类型 × 样式的适用性表（General 全可用；Protein 仅骨架专属样式）
// 行索引 = static_cast<int>(StyleKind)，列索引 = static_cast<int>(RepresentationType)
constexpr bool kStyleSupportsKindTable[static_cast<int>(StyleKind::Count)]
                                       [static_cast<int>(RepresentationType::Count)] = {
    // General（小分子/溶剂/通用）：全部可用
    {true, true, true, true, true, true, true, true, true, true, true, true, true, true,
     true, true, true},
    // Protein：仅骨架专属（Cartoon=4 / Tube=9 / NewCartoon=10 / Ribbon=11）+ 平板/菱形/多面体
    {false, false, false, false, true, false, false, false, false, true, true, true, false,
     true, true, false, true},
};

bool typeIndexValid(RepresentationType t) noexcept {
    const auto i = static_cast<int>(t);
    return t != RepresentationType::Count && i >= 0 && i < static_cast<int>(RepresentationType::Count);
}

bool kindIndexValid(StyleKind k) noexcept {
    const auto i = static_cast<int>(k);
    return k != StyleKind::Count && i >= 0 && i < static_cast<int>(StyleKind::Count);
}

} // namespace

const char* styleDisplayName(RepresentationType type) noexcept {
    return typeIndexValid(type)
               ? kStyleNames[static_cast<int>(type)]
               : nullptr;
}

bool styleSupportsKind(StyleKind kind, RepresentationType type) noexcept {
    if (!kindIndexValid(kind) || !typeIndexValid(type)) {
        return false;
    }
    return kStyleSupportsKindTable[static_cast<int>(kind)][static_cast<int>(type)];
}

RepresentationType defaultStyle(StyleKind kind) noexcept {
    // 蛋白质 → 卡通（骨架表现）；通用/越界 → 球棍（既有默认，§5.2）
    return kind == StyleKind::Protein ? RepresentationType::Cartoon
                                      : RepresentationType::BallAndStick;
}

} // namespace moleculens::engine