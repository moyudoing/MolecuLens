// =============================================================================
// MolecuLens · app · controller/selection/selection_controller.h —— 选择控制器
// 规格来源：主方案 §5.6 / §9（拾取/选择 → SelectionChanged；选择语言求值）
// =============================================================================

#pragma once

#include <string>
#include <vector>

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/picking/picking.h"

namespace moleculens::app {

class SelectionController : public Subscriber {
public:
    explicit SelectionController(AppServices& svc);

    // 拾取回查（pickId → (instance, atom)）；成功发布 SelectionChanged
    bool pick(uint32_t pickId);
    bool pickRay(float ndcX, float ndcY, int viewportW, int viewportH);
    // 选择语言（主方案 §4.6）
    bool select(const std::string& expression);

    const engine::PickResult& current() const { return current_; }
    bool hasSelection() const { return has_; }

    // —— 选择语言求值结果（主方案 §4.6）——
    // mask 按全局原子顺序（与 CoordsSoA / 渲染器的选择掩码同一顺序）；
    // select() 成功后由 SelectionChanged 事件的 payload 携带命中原子数。
    [[nodiscard]] const std::vector<bool>& mask() const { return mask_; }
    [[nodiscard]] size_t selectedCount() const { return selectedCount_; }
    [[nodiscard]] const std::string& expression() const { return expression_; }

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
    engine::PickResult current_;
    bool has_ = false;

    std::vector<bool> mask_;          // 最近一次 select() 的命中掩码
    size_t selectedCount_ = 0;
    std::string expression_;          // 最近一次成功的表达式（"" = 未选择）
};

} // namespace moleculens::app
