// =============================================================================
// MolecuLens · app · controller/render/render_controller.cpp
// =============================================================================

#include "app/controller/render/render_controller.h"

#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

RenderController::RenderController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

bool RenderController::setBackgroundColor(float r, float g, float b) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.bgColor[0] = r;
    props.bgColor[1] = g;
    props.bgColor[2] = b;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    if (svc_.bus) {
        svc_.bus->publish({EventType::StructureChanged, entity::DIRTY_PROPERTIES});
    }
    return true;
}

bool RenderController::setAmbient(float ambient) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().ambient = ambient;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setFog(float near, float far, int mode, float density) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.fog[0] = near;
    props.fog[1] = far;
    props.fogMode = static_cast<float>(mode);
    props.fogDensity = density;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setGlobalScale(float scale) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().scale = scale;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

void RenderController::onEvent(const Event&) {}

} // namespace moleculens::app
