// =============================================================================
// MolecuLens · app · controller/camera/camera_controller.cpp
// =============================================================================

#include "app/controller/camera/camera_controller.h"

#include "engine/camera/camera.h"
#include "engine/render/renderer.h"
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

CameraController::CameraController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

void CameraController::centerOnScene() {
    if (!svc_.camera || !svc_.renderer) {
        return;
    }
    glm::vec3 center;
    float radius = 0.0f;
    if (svc_.renderer->sceneBounds(&center, &radius)) {
        svc_.camera->centerOn(center, radius);
        if (svc_.bus) {
            svc_.bus->publish({EventType::CameraChanged, 0});
        }
    }
}

bool CameraController::setPerspective(bool ortho) {
    if (!svc_.camera) {
        return false;
    }
    svc_.camera->ortho = ortho;
    if (svc_.bus) {
        svc_.bus->publish({EventType::CameraChanged, 0});
    }
    return true;
}

bool CameraController::setFov(float degrees) {
    if (!svc_.camera) {
        return false;
    }
    svc_.camera->setFov(degrees);
    if (svc_.bus) {
        svc_.bus->publish({EventType::CameraChanged, 0});
    }
    return true;
}

void CameraController::orbit(float dx, float dy) {
    if (svc_.camera) {
        svc_.camera->orbit(dx, dy);
    }
}

void CameraController::dolly(float factor) {
    if (svc_.camera) {
        svc_.camera->dolly(factor);
    }
}

void CameraController::pan(float ndcDx, float ndcDy) {
    if (svc_.camera) {
        svc_.camera->pan({ndcDx, ndcDy});
    }
}

void CameraController::rollView(float delta) {
    if (svc_.camera) {
        svc_.camera->rollView(delta);
    }
}

void CameraController::reset() {
    if (svc_.camera) {
        svc_.camera->yaw = 0.0f;
        svc_.camera->pitch = 0.5f;
        svc_.camera->roll = 0.0f;
        svc_.camera->ortho = false;
        svc_.camera->fov = 45.0f;
    }
    centerOnScene();
}

void CameraController::onEvent(const Event& ev) {
    if (ev.type == EventType::SceneOpened) {
        centerOnScene();
    }
}

} // namespace moleculens::app
