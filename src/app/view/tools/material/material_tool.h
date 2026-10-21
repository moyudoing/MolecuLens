// =============================================================================
// Moleculens · app · view/tools/material/material_tool.h —— 材质选择面板
// 规格来源：主方案 §6.6.2/§7.9；材质库见 engine/render/render_parameter.h
//           （23 套，名字与 VMD 的 Material 下拉对齐）
// 说明：预设表**不再定义在本类里** —— 原先这里手写了一份 8 套表，与引擎侧的表
//       是两处事实来源。现统一消费 engine 的 kMaterialPresets（单一事实来源）。
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class RepresentationController;

class MaterialTool : public BaseToolView {
public:
    const char* getTitle() const override { return "材质选择"; }
    // 窗口稳定 ID（与 dock_layout.h 的 panel::kMaterial 必须一致；
    // 它参与 Dock 布局持久化，**不可随语言变化**）
    [[nodiscard]] const char* getStableId() const override { return dock::panel::kMaterial; }
    void render(DataManager* dm, MolScene* scene) override;

    // 控制器注入（ViewControl::svcRep 装配）
    void setRepController(RepresentationController* ctl) { repCtl_ = ctl; }

private:
    RepresentationController* repCtl_ = nullptr;
};

} // namespace moleculens::app
