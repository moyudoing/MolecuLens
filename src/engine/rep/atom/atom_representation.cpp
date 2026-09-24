// =============================================================================
// MolecuLens · engine · rep/atom_representation.cpp —— 原子级表示管理器实现
// =============================================================================

#include "engine/rep/atom/atom_representation.h"

#include <algorithm>
#include <set>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

int32_t AtomRepresentationManager::addGroup(
    const std::string& name,
    const std::vector<uint32_t>& atomIndices,
    RepresentationType type,
    ColorScheme colorScheme) {

    // 检查名称唯一性
    for (const auto& group : groups_) {
        if (group.name == name) {
            spdlog::warn("[rep] Atom group '{}' already exists", name);
            return -1;
        }
    }

    AtomGroupRepresentation group;
    group.name = name;
    group.atomIndices = atomIndices;
    group.type = type;
    group.colorScheme = colorScheme;
    groups_.push_back(group);

    spdlog::info("[rep] Added atom group '{}' with {} atoms", name, atomIndices.size());
    return static_cast<int32_t>(groups_.size() - 1);
}

bool AtomRepresentationManager::removeGroup(int32_t groupId) {
    if (groupId < 0 || groupId >= static_cast<int32_t>(groups_.size())) {
        return false;
    }

    spdlog::info("[rep] Removed atom group '{}'", groups_[groupId].name);
    groups_.erase(groups_.begin() + groupId);
    return true;
}

bool AtomRepresentationManager::updateGroupType(int32_t groupId, RepresentationType type) {
    if (groupId < 0 || groupId >= static_cast<int32_t>(groups_.size())) {
        return false;
    }
    groups_[groupId].type = type;
    return true;
}

bool AtomRepresentationManager::updateGroupColorScheme(int32_t groupId, ColorScheme colorScheme) {
    if (groupId < 0 || groupId >= static_cast<int32_t>(groups_.size())) {
        return false;
    }
    groups_[groupId].colorScheme = colorScheme;
    return true;
}

bool AtomRepresentationManager::updateGroupRadiusScale(int32_t groupId, float scale) {
    if (groupId < 0 || groupId >= static_cast<int32_t>(groups_.size())) {
        return false;
    }
    groups_[groupId].radiusScale = scale;
    return true;
}

bool AtomRepresentationManager::updateGroupVisibility(int32_t groupId, bool visible) {
    if (groupId < 0 || groupId >= static_cast<int32_t>(groups_.size())) {
        return false;
    }
    groups_[groupId].visible = visible;
    return true;
}

std::vector<bool> AtomRepresentationManager::getAtomMask(uint32_t totalAtoms,
                                                          int32_t groupId) const {
    std::vector<bool> mask(totalAtoms, false);

    if (groupId < 0 || groupId >= static_cast<int32_t>(groups_.size())) {
        return mask;
    }

    const auto& group = groups_[groupId];
    for (uint32_t idx : group.atomIndices) {
        if (idx < totalAtoms) {
            mask[idx] = true;
        }
    }

    return mask;
}

int32_t AtomRepresentationManager::createGroupFromSelection(
    const std::string& name,
    const std::string& selectionStr) {

    // 这里可以集成现有的 Selection 解析系统
    // 暂时返回错误，后续可以扩展
    spdlog::warn("[rep] Selection-based atom groups not yet implemented");
    return -1;
}

} // namespace moleculens::engine
