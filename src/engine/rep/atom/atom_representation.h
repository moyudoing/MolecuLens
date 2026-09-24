// =============================================================================
// MolecuLens · engine · rep/atom_representation.h —— 原子级渲染控制
// 功能：支持单个原子或原子组的独立渲染模式切换
// =============================================================================

#pragma once

#include <string>
#include <vector>

#include "engine/rep/representation.h"

namespace moleculens::engine {

// 原子组表示（per-atom representation）
struct AtomGroupRepresentation {
    std::string name;                           // 组名称
    std::vector<uint32_t> atomIndices;          // 原子索引列表
    RepresentationType type = RepresentationType::BallAndStick;
    ColorScheme colorScheme = ColorScheme::ElementCPK;
    float radiusScale = 1.0f;
    bool visible = true;
};

// 原子级表示管理器
class AtomRepresentationManager {
public:
    // 添加原子组表示
    int32_t addGroup(const std::string& name,
                     const std::vector<uint32_t>& atomIndices,
                     RepresentationType type,
                     ColorScheme colorScheme);

    // 移除原子组
    bool removeGroup(int32_t groupId);

    // 更新原子组
    bool updateGroupType(int32_t groupId, RepresentationType type);
    bool updateGroupColorScheme(int32_t groupId, ColorScheme colorScheme);
    bool updateGroupRadiusScale(int32_t groupId, float scale);
    bool updateGroupVisibility(int32_t groupId, bool visible);

    // 获取所有组
    const std::vector<AtomGroupRepresentation>& groups() const { return groups_; }

    // 获取指定组的原子掩码（用于渲染时过滤）
    std::vector<bool> getAtomMask(uint32_t totalAtoms, int32_t groupId) const;

    // 从选择字符串创建组（使用现有的 Selection 系统）
    int32_t createGroupFromSelection(const std::string& name,
                                     const std::string& selectionStr);

private:
    std::vector<AtomGroupRepresentation> groups_;
};

} // namespace moleculens::engine
