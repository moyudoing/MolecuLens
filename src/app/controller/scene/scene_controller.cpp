// =============================================================================
// MolecuLens · app · controller/scene/scene_controller.cpp
// =============================================================================

#include "app/controller/scene/scene_controller.h"

#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

SceneController::SceneController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

uint32_t SceneController::addInstance(uint32_t structureIndex,
                                      const entity::Transform& tm, std::string name) {
    if (!svc_.scene) {
        return UINT32_MAX;
    }
    const uint32_t index = svc_.scene->addInstance(structureIndex, tm);
    // 实例名由 SceneStructure 自动生成（molecule_N）；M2 接入命名编辑
    (void)name;
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, static_cast<int64_t>(index)});
    }
    return index;
}

bool SceneController::removeInstance(uint32_t instanceIndex) {
    if (!svc_.scene || instanceIndex >= svc_.scene->instances().size()) {
        return false;
    }
    svc_.scene->removeInstance(instanceIndex);
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged,
                           static_cast<int64_t>(instanceIndex)});
    }
    return true;
}

bool SceneController::setVisible(uint32_t instanceIndex, bool visible) {
    if (!svc_.scene || instanceIndex >= svc_.scene->instances().size()) {
        return false;
    }
    svc_.scene->setInstanceVisible(instanceIndex, visible);
    svc_.scene->setDirty(entity::DIRTY_INSTANCE);
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged,
                           static_cast<int64_t>(instanceIndex)});
    }
    return true;
}

bool SceneController::setTransform(uint32_t instanceIndex, const entity::Transform& tm) {
    if (!svc_.scene || instanceIndex >= svc_.scene->instances().size()) {
        return false;
    }
    svc_.scene->setInstanceTransform(instanceIndex, tm);
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged,
                           static_cast<int64_t>(instanceIndex)});
    }
    return true;
}

void SceneController::onEvent(const Event& ev) {
    switch (ev.type) {
        case EventType::SceneOpened:
            spdlog::info("[controller] scene opened: {}", ev.payload);
            break;
        default:
            break;
    }
}

} // namespace moleculens::app
