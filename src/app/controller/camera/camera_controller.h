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

    // —— 程序化变更（UI 按钮 / 事件驱动）—— 会广播 CameraChanged
    // 自动取景（加载后调用，主方案 §5.5：radius/tan(fov/2)*1.2）
    void centerOnScene();
    bool setPerspective(bool ortho);
    bool setFov(float degrees);
    void reset();

    // —— 交互式变更（鼠标拖拽，视口每帧调用）—— **不广播** CameraChanged：
    // 订阅方按帧读相机真值即可，每帧广播只会让订阅方做无用功；视口重绘由
    // MolScene 的相机指纹变更检测驱动（见 MolScene::cameraFingerprint）。
    //
    // 本类是改变相机的**唯一入口**：视口交互（MolScene::handleMouse/handleScroll）
    // 与面板控件都必须经由此处，避免出现"绕过控制器直接改相机"的第二条路径。
    void orbit(float dx, float dy);
    void dolly(float factor);
    void pan(float ndcDx, float ndcDy);
    void rollView(float delta);   // 绕屏幕法线滚转（正 = 逆时针）

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
};

} // namespace moleculens::app
