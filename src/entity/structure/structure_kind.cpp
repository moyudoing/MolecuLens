// =============================================================================
// MolecuLens · entity · structure_kind.cpp —— 结构类型分类实现
// 规格来源：主方案 §4.x / §5.7（按残基/链判定分子类型，供渲染样式与 UI 适配）
// =============================================================================

#include "entity/structure/structure_kind.h"

#include "entity/scene/scene.h"

namespace moleculens::entity {

namespace {

// 去掉原子名两端空白（pdb/gro 解析后的名字常带前导/后缀空格，如 " N"/"CA "）
// 与 cartoon.cpp extractBackbones 的判定（"N"/" N"、"CA"/"CA "、"C"/" C"）一致。
std::string_view trimName(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// 是否为溶剂残基名（与 gro_parser.cpp isSolventResidue 保持一致）
bool isSolventResidue(std::string_view r) {
    static const char* kSolventNames[] = {
        "SOL", "HOH", "WAT", "W", "TIP3", "TIP4", "TIP5", "SPC", "SPCE", "SPC/E",
        "CL ", "CLA", "NA+", "K+", "NA", "K", "Mg", "Ca", "Cl-",
    };
    std::string_view t = trimName(r);
    for (const char* n : kSolventNames) {
        if (t == n) return true;
    }
    return false;
}

} // namespace

StructureKind classifyStructure(const SceneStructure& scene, uint32_t structureIndex) {
    // 越界 → 无法判定，归为小分子
    if (structureIndex >= scene.structures().size()) {
        return StructureKind::SmallMol;
    }
    const MoleculeStructure& st = scene.structures()[structureIndex];

    bool hasBackboneN = false, hasBackboneCA = false, hasBackboneC = false;
    bool hasChain = false, hasResidue = false;

    for (size_t i = 0; i < st.elements.size(); ++i) {
        const std::string_view name = trimName(scene.atomName(structureIndex,
                                                              static_cast<uint32_t>(i)));
        if (name == "N") hasBackboneN = true;
        else if (name == "CA") hasBackboneCA = true;
        else if (name == "C") hasBackboneC = true;

        if (st.atoms[i].chainId >= 0) hasChain = true;
        if (st.atoms[i].residueId >= 0) hasResidue = true;
    }

    // 溶剂判定：该结构任一实例标为溶剂，或残基名为溶剂名
    bool isSolvent = false;
    if (hasResidue) {
        for (const auto& inst : scene.instances()) {
            if (inst.structureIndex == structureIndex && inst.isSolvent) {
                isSolvent = true;
                break;
            }
        }
    }
    if (!isSolvent && hasResidue) {
        // 用首个残基名判定
        int32_t rid = -1;
        for (size_t i = 0; i < st.atoms.size(); ++i) {
            if (st.atoms[i].residueId >= 0) { rid = st.atoms[i].residueId; break; }
        }
        if (rid >= 0) {
            const std::string_view rname = scene.residueName(rid);
            if (!rname.empty() && isSolventResidue(rname)) {
                isSolvent = true;
            }
        }
    }
    if (isSolvent) {
        return StructureKind::Solvent;
    }

    // 蛋白判定：具备 backbone 三角 + 至少一个残基（骨架原子在残基内才有意义）
    if (hasBackboneN && hasBackboneCA && hasBackboneC && hasResidue) {
        return StructureKind::Protein;
    }

    return StructureKind::SmallMol;
}

const char* structureKindName(StructureKind kind) noexcept {
    switch (kind) {
        case StructureKind::Protein: return "蛋白";
        case StructureKind::Solvent: return "溶剂";
        case StructureKind::SmallMol:
        default:                     return "小分子";
    }
}

} // namespace moleculens::entity