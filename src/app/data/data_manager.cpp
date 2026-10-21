// =============================================================================
// MolecuLens · app · data/data_manager.cpp —— 数据门面实现
// =============================================================================

#include "app/data/data_manager.h"

#include "entity/scene/scene.h"

namespace moleculens::app {

size_t DataManager::moleculeCount() const {
    return scene_ ? scene_->instances().size() : 0;
}

std::string DataManager::moleculeName(size_t index) const {
    if (!scene_ || index >= scene_->instances().size()) {
        return {};
    }
    return scene_->instances()[index].name;
}

bool DataManager::moleculeVisible(size_t index) const {
    if (!scene_ || index >= scene_->instances().size()) {
        return false;
    }
    return scene_->instances()[index].visible;
}

uint32_t DataManager::moleculeStructureIndex(size_t index) const {
    if (!scene_ || index >= scene_->instances().size()) {
        return UINT32_MAX;
    }
    return scene_->instances()[index].structureIndex;
}

size_t DataManager::atomCount(size_t index) const {
    if (!scene_ || index >= scene_->instances().size()) {
        return 0;
    }
    return scene_->structures()[scene_->instances()[index].structureIndex].elements.size();
}

size_t DataManager::bondCount(size_t index) const {
    if (!scene_ || index >= scene_->instances().size()) {
        return 0;
    }
    return scene_->structures()[scene_->instances()[index].structureIndex].bonds.size();
}

size_t DataManager::frameCount(size_t index) const {
    if (!scene_ || index >= scene_->instances().size() ||
        !scene_->instances()[index].trajectory) {
        return 0;
    }
    return scene_->instances()[index].trajectory->frames.size();
}

std::string DataManager::moleculeTrajectorySource(size_t index) const {
    if (!scene_ || index >= scene_->instances().size() ||
        !scene_->instances()[index].trajectory) {
        return {};
    }
    return scene_->instances()[index].trajectory->source;
}

float DataManager::moleculeTrajectoryDt(size_t index) const {
    if (!scene_ || index >= scene_->instances().size() ||
        !scene_->instances()[index].trajectory) {
        return 0.0f;
    }
    return scene_->instances()[index].trajectory->dt;
}

size_t DataManager::systemCount() const {
    return scene_ ? scene_->systems().size() : 0;
}

std::string DataManager::systemName(size_t systemIndex) const {
    if (!scene_ || systemIndex >= scene_->systems().size()) {
        return {};
    }
    return scene_->systems()[systemIndex].name;
}

bool DataManager::systemVisible(size_t systemIndex) const {
    if (!scene_ || systemIndex >= scene_->systems().size()) {
        return false;
    }
    return scene_->systems()[systemIndex].visible;
}

uint32_t DataManager::systemId(size_t systemIndex) const {
    if (!scene_ || systemIndex >= scene_->systems().size()) {
        return UINT32_MAX;
    }
    return scene_->systems()[systemIndex].id;
}

uint32_t DataManager::systemIdOfInstance(size_t instanceIndex) const {
    if (!scene_) {
        return UINT32_MAX;
    }
    return scene_->systemIdOfInstance(static_cast<uint32_t>(instanceIndex));
}

size_t DataManager::systemMemberCount(size_t systemIndex) const {
    if (!scene_ || systemIndex >= scene_->systems().size()) {
        return 0;
    }
    return scene_->systems()[systemIndex].instanceIndices.size();
}

uint32_t DataManager::systemMemberInstance(size_t systemIndex,
                                           size_t posInSystem) const {
    if (!scene_ || systemIndex >= scene_->systems().size()) {
        return UINT32_MAX;
    }
    const auto& inds = scene_->systems()[systemIndex].instanceIndices;
    if (posInSystem >= inds.size()) {
        return UINT32_MAX;
    }
    return inds[posInSystem];
}

} // namespace moleculens::app
