// =============================================================================
// MolecuLens · app · view/tools/mol_list/mol_list_tool.h —— 分子列表面板（体系树）
// 规格来源：主方案 §7.9；扩展：以体系（组）为单位管理——体系树 + 成员列表 +
//           跨组移交/重组 + 位置（平移/定位）编辑
// =============================================================================

#pragma once

#include <cstdint>

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class SceneController;
class MolScene;

class MolListTool : public BaseToolView {
public:
    const char* getTitle() const override { return "分子列表"; }
    // 窗口稳定 ID（与 dock_layout.h 的 panel::kMolList 必须一致；
    // 它参与 Dock 布局持久化，**不可随语言变化**）
    [[nodiscard]] const char* getStableId() const override { return dock::panel::kMolList; }
    void render(DataManager* dm, MolScene* scene) override;

    // 控制器注入（ViewControl::svcScene 装配）
    void setSceneController(SceneController* ctl) { sceneCtl_ = ctl; }

private:
    SceneController* sceneCtl_ = nullptr;
    size_t selectedSystem_ = 0;       // 选中体系（systems_ 下标）
    int selectedInstance_ = -1;       // 选中分子（实例下标；-1 = 无）
    size_t expandedSystem_ = SIZE_MAX;  // 展开的体系（SIZE_MAX = 全部收起）
    int renameSystemIndex_ = -1;      // 正在重命名的体系下标（-1 = 无）
    char renameBuf_[128] = {};
};

} // namespace moleculens::app
