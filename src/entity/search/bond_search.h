// =============================================================================
// MolecuLens · entity · search/bond_search.h —— 键搜索（均匀网格 cell-list + 多线程）
// 规格来源：主方案 §4.7（判键条件 dist ≤ cov(a)+cov(b)+0.45 Å；
//           cell 尺寸 = 最大判键距离，仅检查 27 邻域 cell；多线程分块 + 互斥合并）
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::entity {

struct BondSearchOptions {
    float tolerance = 0.45f;      // 判键容差（Å）
    bool  multiThreaded = true;
    int   threadCount = 0;        // 0 = 自动（硬件并发）
};

class BondSearch {
public:
    // 由坐标 + 元素推断键表（写入 bonds）；positions 单位 Å
    static void search(const std::vector<glm::vec3>& positions,
                       const std::vector<ElementType>& elements,
                       std::vector<Bond>& outBonds,
                       const BondSearchOptions& opts = {});

    // 便捷：对 SceneStructure 单实例结构补键
    static void searchForStructure(const SceneStructure& scene, uint32_t structureIndex,
                                   const std::vector<glm::vec3>& positions,
                                   const BondSearchOptions& opts = {});
};

} // namespace moleculens::entity
