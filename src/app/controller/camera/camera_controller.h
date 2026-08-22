// =============================================================================
// MolecuLens · app · controller/camera/camera_controller.h —— 相机控制器
// 规格来源：主方案 §5.5（自动取景 centerOn；透视/正交切换；CameraChanged 事件）
// =============================================================================

#pragma once

#include "app/events/event_bus.h"
#include "app/shell/application.h"

namespace moleculens::app {

class CameraController : public Subscriber {
public:
    explicit CameraController(AppServices& svc);

    // 自动取景（加载后调用，主方案 §5.5：radius/tan(fov/2)*1.2）
    void centerOnScene();
    bool setPerspective(bool ortho);
    bool setFov(float degrees);
    void orbit(float dx, float dy);
    void dolly(float factor);
    void pan(float ndcDx, float ndcDy);
    void reset();

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
};

} // namespace moleculens::app
