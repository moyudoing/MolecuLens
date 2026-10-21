// =============================================================================
// Moleculens · app · view/tools/styles/styles_tool.h —— 渲染设置面板（场景级）
// 规格来源：主方案 §7.9（视角/背景/雾/光照；均带重置）
//
// 职责边界：本面板**只**管场景级设置。表示级的一切（样式/配色/原子子集/缩放/
// 材质/QuickSurf 参数/整体系合并表面/表示增删）由「图形表示」面板
// （view/tools/reps/reps_tool.h）唯一负责 —— 同一参数有两个入口会让人以为
// 改的是两个不同的东西，且旧面板的表示"列表"与新面板的表格在视觉上互相矛盾。
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class CameraController;
class RenderController;

class StylesTool : public BaseToolView {
public:
    [[nodiscard]] const char* getTitle() const override { return "渲染设置"; }
    // 窗口稳定 ID（与 dock_layout.h 的 panel::kStyles 必须一致；
    // 它参与 Dock 布局持久化，**不可随语言变化**）
    [[nodiscard]] const char* getStableId() const override { return dock::panel::kStyles; }
    void render(DataManager* dm, MolScene* scene) override;

    // 控制器注入（Application 装配后由 ViewControl::svc* 调用）。
    // 注：不再需要 RepresentationController —— 表示级 UI 已全部迁至「图形表示」。
    void setRenderController(RenderController* ctl) { renderCtl_ = ctl; }
    void setCameraController(CameraController* ctl) { cameraCtl_ = ctl; }

private:
    RenderController* renderCtl_ = nullptr;
    CameraController* cameraCtl_ = nullptr;
};

} // namespace moleculens::app
