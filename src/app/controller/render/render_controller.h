// =============================================================================
// MolecuLens · app · controller/render/render_controller.h —— 渲染控制器
// 规格来源：主方案 §5.8.4（背景/光照/雾/单位变更 → DIRTY_PROPERTIES）
// =============================================================================

#pragma once

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/render/render_parameter.h"   // RenderStyle / RenderStylePreset

namespace moleculens::app {

class RenderController : public Subscriber {
public:
    explicit RenderController(AppServices& svc);

    bool setBackgroundColor(float r, float g, float b);
    bool setAmbient(float ambient);
    bool setFog(float near, float far, int mode, float density);
    bool setGlobalScale(float scale);
    // 光照（写入 SceneProperties → DIRTY_PROPERTIES → 视口重渲染）。
    // 半球环境光：天顶/地面色 + 均匀环境光；方向光：方向/颜色/强度/开关。
    // 方向光为 P6 新增（主方案 §6.6.4），提供造型与高光，让材质参数真实可见。
    bool setSkyColor(float r, float g, float b);
    bool setGroundColor(float r, float g, float b);
    bool setAmbientColor(float r, float g, float b);
    // 方向光方向：由方位角/仰角（弧度）给定（UI 滑杆友好，避免手填三分量）
    bool setDirLightAngles(float azimuth, float elevation);
    bool setDirLightColor(float r, float g, float b);
    bool setDirLightIntensity(float intensity);
    bool setDirLightEnabled(bool enabled);
    // 方向光阴影（深度 pass + PCF）：开关 + 强度（0 = 无阴影，1 = 全黑本影）
    bool setShadowsEnabled(bool enabled);
    bool setShadowStrength(float strength);
    // 当前方向光方位角/仰角（弧度）回读（UI 显示真值）
    void dirLightAngles(float* azimuth, float* elevation) const;

    // —— 渲染风格（v2.2）——
    // 纯 UBO 参数：不重建命令流；设置后下一帧生效。预设同时调整背景/光照/阴影
    // （"出版"预设 = 白底 + 高调柔光 + 无阴影 + 强轮廓），四个滑杆可在预设上微调。
    void setRenderStyle(const engine::RenderStyle& style);
    [[nodiscard]] const engine::RenderStyle& renderStyle() const { return style_; }
    [[nodiscard]] engine::RenderStylePreset stylePreset() const { return stylePreset_; }
    void applyRenderStylePreset(engine::RenderStylePreset preset);

    // —— 剖切面（v2.3）——
    // 世界空间平面 dot(p,n)+d=0；保留 <=0 一侧。offset = 沿法线的可见侧边界。
    bool setClipEnabled(bool enabled);
    bool setClipAxis(int axis, float offset);   // axis: 0..5 = -X,+X,-Y,+Y,-Z,+Z
    bool setClipOffset(float offset);           // 保持当前法线，只移平面
    void clipState(bool* enabled, glm::vec3* normal, float* offset) const;

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
    engine::RenderStyle style_{};
    engine::RenderStylePreset stylePreset_ = engine::RenderStylePreset::Default;
};

} // namespace moleculens::app
