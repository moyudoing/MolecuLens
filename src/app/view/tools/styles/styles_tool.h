// =============================================================================
// MolecuLens · app · view/tools/styles/styles_tool.h —— 渲染设置面板
// 规格来源：主方案 §7.9（样式/着色方案/正交透视/FOV/球体缩放/键半径/
//           背景色/裁剪面/雾效；均带重置）
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class RepresentationController;
class RenderController;

class StylesTool : public BaseToolView {
public:
    [[nodiscard]] const char* getTitle() const override { return "渲染设置"; }
    void render(DataManager* dm, MolScene* scene) override;

    // 控制器注入（Application 装配后由 ViewControl::svc* 调用）
    void setRepController(RepresentationController* ctl) { repCtl_ = ctl; }
    void setRenderController(RenderController* ctl) { renderCtl_ = ctl; }

private:
    RepresentationController* repCtl_ = nullptr;
    RenderController* renderCtl_ = nullptr;
};

} // namespace moleculens::app
