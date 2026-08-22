// =============================================================================
// MolecuLens · app · view/tools/paint/painting_tool.cpp —— 绘图工具占位
// 规格来源：主方案 §7.9（首期 UI 就绪；拾取逻辑 M2 对接 §5.6）
// =============================================================================

#include "app/view/tools/paint/painting_tool.h"

#include "app/view/theme/ui_theme.h"

namespace moleculens::app {

void PaintingTool::render(DataManager* dm, MolScene* scene) {
    (void)dm;
    (void)scene;
    static int mode = 0;
    ImGui::Text("交互模式");
    ImGui::SameLine();
    ImGui::Combo("##mode", &mode, "选择\0测量\0标记\0");
    UITheme::sectionHeader("预留");
    ImGui::TextDisabled("拾取/测量/标记逻辑将在 M2 与拾取系统（§5.6）对接。");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Text("快捷键：");
    ImGui::BulletText("F 聚焦选中 / R 重置相机 / H 切换 HUD");
    ImGui::BulletText("Space 播放暂停 / Shift+←→ 帧步进");
    ImGui::BulletText("Ctrl+O 打开文件");
}

} // namespace moleculens::app
