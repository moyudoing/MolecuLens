// =============================================================================
// MolecuLens · entity · atom/atom.h —— 原子与键（AoS 紧凑布局）
// 规格来源：主方案 §4.1（Atom 紧凑字段；Bond {a,b,order}，0 芳 / 1 / 2 / 3）
// =============================================================================

#pragma once

#include <cstdint>

#include "entity/element/element.h"

namespace moleculens::entity {

struct Atom {
    ElementType element;                 // 元素类型
    uint8_t     occupancy;               // 0–100
    int16_t     bfactor;                 // ×100 存储，避免浮点
    int32_t     residueId;               // 残基索引，-1 无
    int32_t     chainId;                 // 链索引，-1 无
};

struct Bond {
    uint32_t a, b;                       // 原子下标（无向，a < b 约定）
    uint8_t  order;                      // 0 芳 / 1 / 2 / 3
};

} // namespace moleculens::entity
