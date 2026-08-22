// =============================================================================
// MolecuLens · entity · identity/identity.h —— 结构识别码（StructureKey）
// 规格来源：主方案 §4.2（元素直方图 + 图规范哈希；WL 着色迭代）
// 实现要求：同构分子哈希相同、非拓扑异构哈希不同、水分子×N 去重命中
// =============================================================================

#pragma once

#include <array>
#include <cstdint>

#include "entity/element/element.h"

namespace moleculens::entity {

struct MoleculeStructure;

struct StructureKey {
    std::array<uint32_t, 119> elementHistogram{};  // 元素数量直方图（先比数量）
    uint64_t topologyHash = 0;                     // 图规范哈希（再比种类+拓扑）
    // C++17 手写相等（默认比较需 C++20）
    bool operator==(const StructureKey& other) const {
        return topologyHash == other.topologyHash &&
               elementHistogram == other.elementHistogram;
    }
    bool operator!=(const StructureKey& other) const { return !(*this == other); }
};

struct StructureKeyHash {
    size_t operator()(const StructureKey& k) const noexcept {
        // 组合直方图与拓扑哈希（FNV-1a 混合）
        uint64_t h = 1469598103934665603ull ^ k.topologyHash;
        h *= 1099511628211ull;
        for (uint32_t c : k.elementHistogram) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};

// 图规范哈希（WL 着色迭代，主方案 §4.2）：
//   1. 元素直方图；2. 初始着色 = 元素编号；3. 2–3 轮迭代着色
//      （hash(自身标签, 排序邻居标签多重集)）；4. 标签计数向量 → FNV-1a
StructureKey computeStructureKey(const MoleculeStructure& ms);

} // namespace moleculens::entity
