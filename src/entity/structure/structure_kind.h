// =============================================================================
// MolecuLens · entity · structure_kind.h —— 结构类型分类（蛋白/小分子/溶剂）
// 规格来源：主方案 §4.x / §5.7（按残基/链判定分子类型，供渲染样式与 UI 适配）
// 依赖约束：仅 <cstdint> 与既有实体头
// =============================================================================

#pragma once

#include <cstdint>

namespace moleculens::entity {

class SceneStructure;

// 结构类型分类（数据层判定；渲染层据此选择适用样式，见 engine/style/style_registry）
enum class StructureKind : uint8_t {
    SmallMol = 0,  // 小分子 / 配体 / 通用有机分子
    Protein,       // 蛋白质 / 多肽：具备 backbone（N/CA/C 骨架，有残基/链）
    Solvent,       // 溶剂（水/离子，残基名为 SOL/HOH/WAT… 或 isSolvent 标记）
    Count,
};

// 判定给定场景中下标为 structureIndex 的结构所属类型。
// 规则（与 gro/pdb 解析器的溶剂标记、cartoon 的 backbone 提取一致）：
//   1. 该结构对应实例是溶剂（instance.isSolvent）或残基名为溶剂名 → Solvent
//   2. 具备 backbone 三角（存在 N/CA/C 骨架原子，且至少一个残基/链） → Protein
//   3. 否则 → SmallMol
// structureIndex 越界返回 SmallMol。
StructureKind classifyStructure(const SceneStructure& scene, uint32_t structureIndex);

// 中文显示名（UI/日志徽章用）；越界返回空串
const char* structureKindName(StructureKind kind) noexcept;

} // namespace moleculens::entity