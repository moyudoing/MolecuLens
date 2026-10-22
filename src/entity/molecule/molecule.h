// =============================================================================
// MolecuLens · entity · molecule/molecule.h —— 分子结构（拓扑 + 原子元数据）
// 规格来源：主方案 §4.2（elements 按索引对应 Atom；bonds；key）
// 扩展：atoms/atomNameIds 供选择语言与拾取回查使用（§4.6/§5.6）
// =============================================================================

#pragma once

#include <vector>

#include "entity/atom/atom.h"
#include "entity/element/element.h"
#include "entity/identity/identity.h"

namespace moleculens::entity {

struct MoleculeStructure {
    std::vector<ElementType> elements;       // 按索引对应 Atom（§4.2 原样）
    std::vector<Atom>        atoms;          // 原子元数据（occupancy/bfactor/残基/链）
    std::vector<uint32_t>    atomNameIds;    // 原子名 → SceneStructure::strings()（选择语言）
    std::vector<Bond>        bonds;
    // 每个原子所属连通分量（按键连接；无键原子自成一组）的最小原子下标。
    // 逐帧 PBC 补齐按"分子"而不是"整个体系"锚定，避免一个分子跨盒边界时被
    // 拉成一根贯穿盒子的长棍。registerStructure 时算好，逐帧 O(N) 复用。
    std::vector<uint32_t>    unwrapRoot;
    // 键邻接（CSR）：unwrapAdjOffset[i]..unwrapAdjOffset[i+1] 是原子 i 的邻居。
    // 逐帧 PBC 补齐沿键做 BFS，而不是所有原子都向分量根取最小镜像；后者对
    // 长链/环状分子仍可能把某个原子选到盒子的另一侧，形成贯穿盒子的长键。
    std::vector<uint32_t>    unwrapAdjOffset;
    std::vector<uint32_t>    unwrapAdj;
    StructureKey key;
};

} // namespace moleculens::entity
