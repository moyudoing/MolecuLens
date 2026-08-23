// =============================================================================
// MolecuLens · app · controller/render/render_controller.h —— 渲染控制器
// 规格来源：主方案 §5.8.4（背景/光照/雾/单位变更 → DIRTY_PROPERTIES）
// =============================================================================

#pragma once

#include "app/events/event_bus.h"
#include "app/shell/application.h"

namespace moleculens::app {

class RenderController : public Subscriber {
public:
    explicit RenderController(AppServices& svc);

    bool setBackgroundColor(float r, float g, float b);
    bool setAmbient(float ambient);
    bool setFog(float near, float far, int mode, float density);
    bool setGlobalScale(float scale);
    // 光照（写入 SceneProperties → DIRTY_PROPERTIES → 视口重渲染）
    // 全局光方案：天顶/地面半球色 + 均匀环境光（无方向光/点光源）
    bool setSkyColor(float r, float g, float b);
    bool setGroundColor(float r, float g, float b);
    bool setAmbientColor(float r, float g, float b);

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
};

} // namespace moleculens::app
