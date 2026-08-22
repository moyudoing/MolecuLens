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
    StructureKey key;
};

} // namespace moleculens::entity
