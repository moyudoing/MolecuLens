// =============================================================================
// MolecuLens · app · controller/scene/scene_controller.h —— 场景控制器
// 规格来源：主方案 §3.3 / §5.8（实例增删/显隐/位姿 → 置脏 + 事件广播）
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

#include "app/events/event_bus.h"
#include "app/shell/application.h"

namespace moleculens::entity {
class SceneStructure;
struct Transform;
} // namespace moleculens::entity

namespace moleculens::app {

class SceneController : public Subscriber {
public:
    explicit SceneController(AppServices& svc);

    // 增删/显隐/位姿（成功后发布 MoleculeChanged）
    uint32_t addInstance(uint32_t structureIndex, const entity::Transform& tm,
                         std::string name);
    bool removeInstance(uint32_t instanceIndex);
    bool setVisible(uint32_t instanceIndex, bool visible);
    bool setTransform(uint32_t instanceIndex, const entity::Transform& tm);

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
};

} // namespace moleculens::app
