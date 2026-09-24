// =============================================================================
// MolecuLens · app · shell/application.h —— 组合根（Application）
// 规格来源：主方案 §3.3（P7：控制器拆分，组合根装配，控制器间只经事件总线；
//           依赖注入不暴露全局变量）
// =============================================================================

#pragma once

#include <memory>

#include "app/events/event_bus.h"
#include "entity/scene/scene.h"   // SceneStructure 完整类型（值成员 scene_）

namespace moleculens::engine {
class Renderer;
class VulkanRenderer;
class OrbitCamera;
class FrameScheduler;
} // namespace moleculens::engine

namespace moleculens::app {

class ViewControl;
class DataManager;
class SceneController;
class RenderController;
class RepresentationController;
class SelectionController;
class TimelineController;
class CameraController;
class IoController;

// 控制器服务注入（组合根装配；主方案 §3.3 依赖注入）
struct AppServices {

    entity::SceneStructure* scene = nullptr;
    engine::Renderer* renderer = nullptr;
    engine::VulkanRenderer* vkRenderer = nullptr;
    engine::OrbitCamera* camera = nullptr;
    engine::FrameScheduler* scheduler = nullptr;
    EventBus* bus = nullptr;
    DataManager* data = nullptr;
    ViewControl* view = nullptr;

};

class Application {
public:
    Application();
    ~Application();

    int run(int argc, char** argv);        // 主循环（ViewControl::runLoop）

private:
    bool init();                           // 组合根装配
    void shutdown();
    engine::VulkanRenderer* vkRenderer();

    EventBus bus_;
    entity::SceneStructure scene_;
    std::unique_ptr<engine::Renderer> renderer_;
    std::unique_ptr<engine::OrbitCamera> camera_;
    std::unique_ptr<engine::FrameScheduler> scheduler_;
    AppServices services_;

    std::unique_ptr<SceneController> sceneCtl_;
    std::unique_ptr<RenderController> renderCtl_;
    std::unique_ptr<RepresentationController> repCtl_;
    std::unique_ptr<SelectionController> selectionCtl_;
    std::unique_ptr<TimelineController> timelineCtl_;
    std::unique_ptr<CameraController> cameraCtl_;
    std::unique_ptr<IoController> ioCtl_;
    std::unique_ptr<DataManager> dataMgr_;
    std::unique_ptr<ViewControl> view_;
};

} // namespace moleculens::app
