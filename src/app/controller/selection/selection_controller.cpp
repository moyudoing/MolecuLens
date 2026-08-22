// =============================================================================
// MolecuLens · app · controller/selection/selection_controller.cpp
// =============================================================================

#include "app/controller/selection/selection_controller.h"

#include "engine/camera/camera.h"
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

SelectionController::SelectionController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

bool SelectionController::pick(uint32_t pickId) {
    if (!svc_.scene) {
        return false;
    }
    current_ = engine::resolvePickId(*svc_.scene, pickId);
    has_ = current_.hit;
    if (svc_.bus) {
        svc_.bus->publish({EventType::SelectionChanged,
                           current_.hit ? static_cast<int64_t>(current_.globalAtomId) : -1});
    }
    return current_.hit;
}

bool SelectionController::pickRay(float ndcX, float ndcY, int viewportW, int viewportH) {
    if (!svc_.scene || !svc_.camera) {
        return false;
    }
    // 屏幕 NDC → 世界射线（主方案 §5.6 CPU 兜底）
    const float aspect = viewportW > 0 && viewportH > 0
                             ? static_cast<float>(viewportW) / viewportH
                             : 1.0f;
    const float tanHalf = std::tan(glm::radians(svc_.camera->fov) * 0.5f);
    const glm::vec3 camPos = svc_.camera->position();
    const glm::mat4 view = svc_.camera->viewMatrix();
    const glm::vec3 right = glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0]));
    const glm::vec3 up = glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1]));
    const glm::vec3 fwd = glm::normalize(glm::vec3(-view[0][2], -view[1][2], -view[2][2]));
    const glm::vec3 dir = glm::normalize(fwd + right * (ndcX * tanHalf * aspect) +
                                         up * (ndcY * tanHalf));
    current_ = engine::PickRay::pickScene(*svc_.scene, camPos, dir);
    has_ = current_.hit;
    if (svc_.bus) {
        svc_.bus->publish({EventType::SelectionChanged,
                           current_.hit ? static_cast<int64_t>(current_.globalAtomId) : -1});
    }
    return current_.hit;
}

bool SelectionController::select(const std::string& expression) {
    // M2 完整：选择语言求值 → 高亮节点（当前记录表达式）
    spdlog::debug("[controller] selection expression: {}", expression);
    return true;
}

void SelectionController::onEvent(const Event& ev) {
    if (ev.type == EventType::SelectionChanged) {
        spdlog::debug("[controller] selection changed: {}", ev.payload);
    }
}

} // namespace moleculens::app
