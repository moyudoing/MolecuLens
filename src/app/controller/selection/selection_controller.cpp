// =============================================================================
// MolecuLens · app · controller/selection/selection_controller.cpp
// =============================================================================

#include "app/controller/selection/selection_controller.h"

#include <algorithm>

#include "engine/camera/camera.h"
#include "entity/scene/scene.h"
#include "entity/selection/parse_table.h"
#include "entity/selection/selection.h"
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
    // 屏幕 NDC → 世界射线（射线数学归相机，见 OrbitCamera::rayForNdc）
    const float aspect = viewportW > 0 && viewportH > 0
                             ? static_cast<float>(viewportW) / viewportH
                             : 1.0f;
    glm::vec3 origin{0.0f}, dir{0.0f, 0.0f, -1.0f};
    svc_.camera->rayForNdc(ndcX, ndcY, aspect, &origin, &dir);
    current_ = engine::PickRay::pickScene(*svc_.scene, origin, dir);
    has_ = current_.hit;
    if (svc_.bus) {
        svc_.bus->publish({EventType::SelectionChanged,
                           current_.hit ? static_cast<int64_t>(current_.globalAtomId) : -1});
    }
    return current_.hit;
}

bool SelectionController::select(const std::string& expression) {
    if (!svc_.scene) {
        return false;
    }
    // 选择语言：解析 → 对全场景求值 → 全局原子掩码（主方案 §4.6）
    entity::Selection sel;
    const entity::ParseTable& table = entity::ParseTable::defaultTable();
    if (!sel.parse(expression, table)) {
        spdlog::warn("[controller] 选择表达式语法错误: {}", expression);
        return false;
    }
    // 坐标来源与渲染器构建选择掩码时一致：场景 CoordsSoA 的当前位置
    entity::FrameData frame;
    frame.positions = &svc_.scene->coordsSoA().positions;
    frame.timestep = nullptr;
    sel.evaluate(*svc_.scene, frame, 0);

    mask_ = sel.mask();
    selectedCount_ = static_cast<size_t>(
        std::count(mask_.begin(), mask_.end(), true));
    expression_ = expression;

    if (svc_.bus) {
        // payload = 命中原子数（订阅方据此刷新状态栏/高亮）
        svc_.bus->publish({EventType::SelectionChanged,
                           static_cast<int64_t>(selectedCount_)});
    }
    spdlog::info("[controller] 选择 \"{}\" 命中 {} 个原子", expression, selectedCount_);
    return selectedCount_ > 0;
}

void SelectionController::onEvent(const Event& ev) {
    if (ev.type == EventType::SelectionChanged) {
        spdlog::debug("[controller] selection changed: {}", ev.payload);
    }
}

} // namespace moleculens::app
