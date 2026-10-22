// =============================================================================
// Moleculens · app · view/tools/reps/reps_tool.h —— 图形表示面板
// 规格来源：需求附图（图形表示对话框）
//   顶部：Selected Molecule + Create/Delete Rep + 表示表格(Style/Color/Selection)
//         + Selected Atoms
//   页签：Draw style / Selections / Trajectory / Periodic
//
// 与 styles_tool 的分工：本面板负责**表示级**的增删与全部参数；styles_tool 只保留
// 场景级设置（背景/雾/光照/视角）—— 两处都能改表示就是又一处"两份状态"。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

#include "app/view/tool/base_tool_view.h"
#include "engine/rep/representation.h"

namespace moleculens::app {

class RepresentationController;
class RenderController;
class TimelineController;
class IoController;

class RepsTool : public BaseToolView {
public:
    [[nodiscard]] const char* getTitle() const override { return "图形表示"; }
    // 窗口稳定 ID（与 dock_layout.h 的 panel::kReps 必须一致；
    // 它参与 Dock 布局持久化，**不可随语言变化**）
    [[nodiscard]] const char* getStableId() const override { return dock::panel::kReps; }
    void render(DataManager* dm, MolScene* scene) override;

    void setRepController(RepresentationController* ctl) { repCtl_ = ctl; }
    void setTimelineController(TimelineController* ctl) { timelineCtl_ = ctl; }
    // IoController：ESP 参数面板的"当前加载文件"默认值 + cube 文件打开对话框
    void setIoController(IoController* ctl) { ioCtl_ = ctl; }

private:
    void renderMoleculeBar(DataManager* dm, MolScene* scene);
    void renderRepTable();
    void renderDrawStyleTab(int32_t repId);
    void renderSelectionsTab(int32_t repId);
    void renderTrajectoryTab();
    void renderPeriodicTab(MolScene* scene);
    // ESP 映射表面参数控件（Draw style 页签，type == EspSurface 时显示）。
    // 返回 true = 本帧有参数被改动（调用方据此走 onChange 提交/暂存）。
    bool renderEspParams(engine::Representation& edit);

    // 把 token 追加到当前表示的选择表达式（and/or/not 与 singleword/关键字按钮共用）
    void appendToSelection(int32_t repId, const char* token);

    RepresentationController* repCtl_ = nullptr;
    TimelineController* timelineCtl_ = nullptr;
    IoController* ioCtl_ = nullptr;

    int moleculeIdx_ = 0;        // Selected Molecule 下拉下标（0 = 全部）
    // 由 moleculeIdx_ 推导出的作用域选择表达式（Create Rep 时使用）
    std::string scopedSel_;
    int32_t deleteTarget_ = -1;  // 待删除的表示下标（本帧末尾统一处理）

    // Selected Atoms 编辑缓冲（与模型同步规则同 styles_tool：仅"来源变化"时覆盖）
    char atomsBuf_[256] = {};
    int32_t atomsBufRep_ = -1;
    std::string atomsBufSource_;
    bool atomsBufValid_ = true;

    // Selections 页签
    int keywordIdx_ = 0;         // Keyword 列表下标（表来自 selectionKeywords）
    char valueBuf_[96] = {};
    int singleWordIdx_ = 0;
    char macroName_[64] = {};

    // Draw style 页签
    bool autoApply_ = true;      // Apply Changes Automatically
    // 关闭自动应用时的暂存副本（开启时不存在第二份状态，直接提交给控制器）
    engine::Representation staged_;
    int32_t stagedRep_ = -1;
    bool stagedDirty_ = false;
};

} // namespace moleculens::app
