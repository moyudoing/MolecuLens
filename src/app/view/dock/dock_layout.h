// =============================================================================
// MolecuLens · app · view/dock/dock_layout.h —— Dock 布局构建与序列化
// 规格来源：主方案 §7.4（默认布局：右 25% / 下 10% / 右下再分 55%）、
//           §12.2（布局持久化 base64(imgui ini)）
//
// 从 view_control.cpp 抽出的独立关注点：**布局的构建与持久化**不依赖窗口生命周期、
// 主循环或任何控制器；此前全部堆在 ViewControl 里（近千行的 god object）。
// =============================================================================

#pragma once

#include <string>

#include <imgui.h>

#include "app/view/dock/base64.h"   // base64Encode/base64Decode（头文件实现，可单测）

namespace moleculens::app::dock {

// =============================================================================
// 面板窗口的"显示名 / 稳定 ID"分离
// =============================================================================
// 为什么必须分离：ImGui 的窗口 ID 由窗口名推导，而 **Dock 布局与 imgui.ini 的
// 持久化都以该 ID 为键**。若直接拿显示名当窗口名，一旦显示名变化（切语言、
// 改文案），同一个面板会被当成**新窗口**：停靠位置丢失、面板尺寸/状态重置，
// 已保存的布局也全部失配。
// 故用 ImGui 的 "###" 语法：`显示名###稳定id` —— 显示名可随语言变，ID 恒定。
//
// 面板 ID 常量集中于此：BaseToolView（构造窗口标签）与 buildDefaultLayout
// （DockBuilderDockWindow）必须用**同一份**，否则停靠会静默失效。
namespace panel {
inline constexpr const char* kViewport  = "ml_viewport";
inline constexpr const char* kMolList   = "ml_panel_mol_list";
inline constexpr const char* kStyles    = "ml_panel_styles";
inline constexpr const char* kMaterial  = "ml_panel_material";
inline constexpr const char* kExport    = "ml_panel_export";
inline constexpr const char* kPaint     = "ml_panel_paint";
inline constexpr const char* kTheme     = "ml_panel_theme";
inline constexpr const char* kReps      = "ml_panel_reps";
inline constexpr const char* kTimeline  = "ml_panel_timeline";
inline constexpr const char* kStatusBar = "ml_panel_statusbar";
inline constexpr const char* kLog       = "ml_panel_log";
} // namespace panel

// 窗口标签：显示名（可翻译）+ "###稳定 id"
inline std::string windowLabel(const char* displayName, const char* stableId) {
    return std::string(displayName != nullptr ? displayName : "") + "###" +
           (stableId != nullptr ? stableId : "");
}

// 仅 ID 形式：DockBuilderDockWindow 用。ImGui 对 "###id" 与 "显示名###id"
// 解析出的窗口 ID **相同**，故停靠与窗口创建能对上。
inline std::string windowIdOnly(const char* stableId) {
    return windowLabel("", stableId);
}

// 默认 Dock 布局（主方案 §7.4）：在 dockspaceId 上重建节点树并挂载各面板。
//   workSize = DockSpace 的实际内容区尺寸（调用方用 GetContentRegionAvail 取得——
//              用窗口尺寸会含内边距，导致右/下溢出）
// 仅应在"尚未初始化布局"时调用一次。
void buildDefaultLayout(ImGuiID dockspaceId, const ImVec2& workSize);

// 布局 → base64（无 ImGui 上下文时返回空串）
std::string serializeLayout();
// base64 → 布局，成功返回 true。**不改变调用方的"已初始化"标志**——
// 由调用方置 false 以强制按恢复的布局重建 Dock 节点。
bool deserializeLayout(const std::string& base64);

} // namespace moleculens::app::dock
