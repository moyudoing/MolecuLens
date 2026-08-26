// =============================================================================
// MolecuLens · app · controller/rep/representation_controller.cpp
// =============================================================================

#include "app/controller/rep/representation_controller.h"

#include "engine/render/renderer.h"
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

RepresentationController::RepresentationController(AppServices& svc) : svc_(svc) {

    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }

    // 默认表示（主方案 §7.7：加载即显示球棍）
    reps_.push_back(engine::Representation{});
    reps_.back().name = "ballstick";
    reps_.back().type = engine::RepresentationType::BallAndStick;
}

int32_t RepresentationController::addRepresentation(const std::string& name,
                                                    engine::RepresentationType type,
                                                    const std::string& selection,
                                                    engine::ColorScheme scheme) {
    engine::Representation rep;
    rep.name = name.empty() ? ("rep_" + std::to_string(reps_.size())) : name;
    rep.type = type;
    rep.selection = selection;
    rep.colorScheme = scheme;
    reps_.push_back(rep);
    touch();
    return static_cast<int32_t>(reps_.size()) - 1;
}

bool RepresentationController::removeRepresentation(int32_t repId) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_.erase(reps_.begin() + repId);
    touch();
    return true;
}

bool RepresentationController::setType(int32_t repId, engine::RepresentationType type) {

    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].type = type;
    touch();
    return true;

}

bool RepresentationController::setSelection(int32_t repId, const std::string& selection) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].selection = selection;
    touch();
    return true;
}

bool RepresentationController::setColorScheme(int32_t repId, engine::ColorScheme scheme) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].colorScheme = scheme;
    touch();
    return true;
}

bool RepresentationController::setRadiusScale(int32_t repId, float scale) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].radiusScale = scale;
    touch();
    return true;
}

bool RepresentationController::setBondScale(int32_t repId, float scale) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].bondScale = scale;
    touch();
    return true;
}

bool RepresentationController::setVisible(int32_t repId, bool visible) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].visible = visible;
    touch();
    return true;
}

bool RepresentationController::setSurfIsovalue(int32_t repId, float v) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].surfIsovalue = v;
    touch();
    return true;
}

bool RepresentationController::setSurfGridSpacing(int32_t repId, float s) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].surfGridSpacing = s;
    touch();
    return true;
}

bool RepresentationController::setSystemSurface(int32_t repId, bool on) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].systemSurface = on;
    touch();
    return true;
}

bool RepresentationController::setMaterial(int32_t repId, const float* ambient,
                                           const float* diffuse, const float* specular,
                                           float shininess, float metallic,
                                           float roughness, float opacity) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    auto& rep = reps_[static_cast<size_t>(repId)];
    for (int i = 0; i < 4; ++i) {
        rep.ambient[i] = ambient[i];
        rep.diffuse[i] = diffuse[i];
        rep.specular[i] = specular[i];
    }
    rep.shininess = shininess;
    rep.metallic = metallic;
    rep.roughness = roughness;
    rep.opacity = opacity;
    touch();
    return true;
}

const std::vector<engine::Representation>&
RepresentationController::representations() const {
    return reps_;
}

void RepresentationController::touch() {
    if (svc_.renderer) {
        svc_.renderer->setRepresentations(reps_);
    }
    if (svc_.scene) {
        svc_.scene->setDirty(entity::DIRTY_REPRESENTATION);
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::RepresentationChanged, 0});
    }
}

void RepresentationController::onEvent(const Event&) {}

} // namespace moleculens::app
