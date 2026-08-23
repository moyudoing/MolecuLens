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

bool SceneController::setInstancePosition(uint32_t instanceIndex,
                                          const glm::vec3& position) {
    if (!svc_.scene || instanceIndex >= svc_.scene->instances().size()) {
        return false;
    }
    entity::Transform tm = svc_.scene->instances()[instanceIndex].transform;
    tm.translation = position;
    return setTransform(instanceIndex, tm);
}

bool SceneController::setSystemPosition(uint32_t systemId, const glm::vec3& position) {
    if (!svc_.scene) {
        return false;
    }
    const uint32_t si = svc_.scene->systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    entity::Transform tm = svc_.scene->systems()[si].transform;
    tm.translation = position;
    return setSystemTransform(systemId, tm);
}

uint32_t SceneController::addSystem(const std::string& name) {
    if (!svc_.scene) {
        return UINT32_MAX;
    }
    const uint32_t id = svc_.scene->addSystem(name);
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    return id;
}

bool SceneController::removeSystem(uint32_t systemId) {
    if (!svc_.scene) {
        return false;
    }
    if (!svc_.scene->removeSystem(systemId)) {
        return false;
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    return true;
}

bool SceneController::setSystemVisible(uint32_t systemId, bool visible) {
    if (!svc_.scene) {
        return false;
    }
    if (!svc_.scene->setSystemVisible(systemId, visible)) {
        return false;
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    return true;
}

bool SceneController::setSystemTransform(uint32_t systemId, const entity::Transform& tm) {
    if (!svc_.scene) {
        return false;
    }
    if (!svc_.scene->setSystemTransform(systemId, tm)) {
        return false;
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    return true;
}

bool SceneController::renameSystem(uint32_t systemId, const std::string& name) {
    if (!svc_.scene) {
        return false;
    }
    if (!svc_.scene->renameSystem(systemId, name)) {
        return false;
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    return true;
}

bool SceneController::moveInstanceToSystem(uint32_t instanceIndex, uint32_t systemId) {
    if (!svc_.scene) {
        return false;
    }
    if (!svc_.scene->moveInstanceToSystem(instanceIndex, systemId)) {
        return false;
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged,
                           static_cast<int64_t>(instanceIndex)});
    }
    return true;
}

bool SceneController::setSystemRenderMode(uint32_t systemId, int mode) {
    if (!svc_.scene) {
        return false;
    }
    if (!svc_.scene->setSystemRenderMode(
            systemId, static_cast<entity::SystemRenderMode>(mode))) {
        return false;
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::MoleculeChanged, 0});
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
