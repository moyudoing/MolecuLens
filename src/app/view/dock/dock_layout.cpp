// =============================================================================
// MolecuLens · app · view/dock/dock_layout.cpp —— Dock 布局构建与序列化实现
// 规格来源：主方案 §7.4 / §12.2
// =============================================================================

#include "app/view/dock/dock_layout.h"

#include <imgui_internal.h>   // DockBuilder 系列

#include "spdlog/spdlog.h"

namespace moleculens::app::dock {

void buildDefaultLayout(ImGuiID dockspaceId, const ImVec2& workSize) {
    // 默认布局（主方案 §7.4）：右 25%，下 10%，右下再分 55%
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, workSize);

    ImGuiID right =
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right, 0.25f, nullptr, &dockspaceId);
    ImGuiID bot =
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.10f, nullptr, &dockspaceId);
    ImGuiID rightBot =
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);

    // 3D 主视口：中央节点禁用标签栏（NoTabBar）——上方不再显示窗口标签/功能栏
    //（配合视口窗口 NoTitleBar），主视图沉浸式（主方案 §7.4）。
    // 全部用**稳定 ID**（不含可翻译的显示名）：显示名随语言变化时，这里的 ID 不变，
    // 停靠关系才不会被切断（详见 dock_layout.h 的说明）。
    auto dockPanel = [](const char* stableId, ImGuiID node) {
        ImGui::DockBuilderDockWindow(windowIdOnly(stableId).c_str(), node);
    };
    dockPanel(panel::kViewport, dockspaceId);
    if (ImGuiDockNode* viewNode = ImGui::DockBuilderGetNode(dockspaceId)) {
        viewNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    }
    dockPanel(panel::kMolList, right);
    dockPanel(panel::kStyles, rightBot);
    dockPanel(panel::kTheme, rightBot);
    dockPanel(panel::kMaterial, rightBot);
    dockPanel(panel::kExport, rightBot);
    dockPanel(panel::kPaint, rightBot);
    dockPanel(panel::kReps, rightBot);
    dockPanel(panel::kTimeline, bot);
    dockPanel(panel::kStatusBar, bot);
    dockPanel(panel::kLog, bot);
    ImGui::DockBuilderFinish(dockspaceId);
}

std::string serializeLayout() {
    // base64(imgui_dock_serialized)，主方案 §12.2
    if (ImGui::GetCurrentContext() == nullptr) {
        return {};
    }
    size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    if (ini == nullptr || size == 0) {
        return {};
    }
    return base64Encode(std::string(ini, size));
}

bool deserializeLayout(const std::string& base64) {
    if (base64.empty() || ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    std::string raw;
    if (!base64Decode(base64, &raw)) {
        return false;
    }
    // 恢复布局（下次 ImGui::NewFrame 后生效）
    ImGui::LoadIniSettingsFromMemory(raw.c_str(), raw.size());
    spdlog::info("[ui] layout restored from settings ({} bytes)", raw.size());
    return true;
}

} // namespace moleculens::app::dock
