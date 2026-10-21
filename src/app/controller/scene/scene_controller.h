// =============================================================================
// MolecuLens · app · controller/scene/scene_controller.h —— 场景控制器
// 规格来源：主方案 §3.3 / §5.8（实例增删/显隐/位姿 → 置脏 + 事件广播）
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

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
    // 平移/定位（读当前位姿、仅改平移分量 → setTransform）
    bool setInstancePosition(uint32_t instanceIndex, const glm::vec3& position);
    bool setSystemPosition(uint32_t systemId, const glm::vec3& position);

    // 体系（组）操作
    uint32_t addSystem(const std::string& name);
    bool removeSystem(uint32_t systemId);
    bool setSystemVisible(uint32_t systemId, bool visible);
    // 卸载该体系的轨迹（成员实例的 trajectory 置空，退回静态单帧）
    bool clearSystemTrajectory(uint32_t systemId);
    bool setSystemTransform(uint32_t systemId, const entity::Transform& tm);
    bool renameSystem(uint32_t systemId, const std::string& name);
    bool moveInstanceToSystem(uint32_t instanceIndex, uint32_t systemId);
    bool setSystemRenderMode(uint32_t systemId, int mode);   // 0实心/1线框/2透明/3点

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
};

} // namespace moleculens::app
