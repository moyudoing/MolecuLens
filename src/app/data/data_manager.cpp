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

} // namespace moleculens::app
