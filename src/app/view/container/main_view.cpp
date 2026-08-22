// =============================================================================
// MolecuLens · app · view/container/main_view.cpp —— 主区域容器实现
// 规格来源：主方案 §7.7（视口交互接缝：IsItemHovered 在 Image() 之后判断）
// =============================================================================

#include "app/view/container/main_view.h"

#include <cstdio>

#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/timeline/time_axis.h"
#include "app/view/viewport/mol_scene.h"

namespace moleculens::app {

MainView::~MainView() = default;

void MainView::renderAll(RenderContext& ctx) {
    renderViewport(ctx);
    if (ctx.showTimeline && *ctx.showTimeline) {
        renderTimeline(ctx);
    }
    if (ctx.showStatusBar && *ctx.showStatusBar) {
        renderStatusBar(ctx);
    }
}

void MainView::renderViewport(RenderContext& ctx) {
    ImGui::Begin("3D 视口", nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // 取可用区 → molScene_->resizeViewport() 同步 FBO → render()（主方案 §7.7）
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // 背景色：深蓝黑底（C_BG）
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::C_BG);
    ImGui::BeginChild("viewport_canvas", avail, false);
    ctx.scene->render(ImGui::GetContentRegionAvail());
    // 命中判断（Image() 之后；区分左/中/右键，主方案 §7.7）
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        // 拖拽（左键轨道 / 中键缩放 / 右键平移）
        for (int btn = 0; btn < 3; ++btn) {
            if (io.MouseDown[btn] && io.MouseDelta.x != 0.0f) {
                ctx.scene->handleMouse(io.MouseDelta.x, io.MouseDelta.y, btn);
            }
        }
        if (io.MouseWheel != 0.0f) {
            ctx.scene->handleScroll(io.MouseWheel);
        }
        if (io.MouseClicked[0]) {
            ctx.scene->handleClick();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
}

void MainView::renderTimeline(RenderContext& ctx) {
    ImGui::Begin("时间轴", ctx.showTimeline, ImGuiWindowFlags_NoCollapse);
    ctx.timeAxis->render();
    ImGui::End();
}

void MainView::renderStatusBar(RenderContext& ctx) {
    ImGui::Begin("状态栏", ctx.showStatusBar, ImGuiWindowFlags_NoCollapse);
    char buf[256];
    const size_t count = ctx.dm ? ctx.dm->moleculeCount() : 0;
    const std::string cur =
        count > 0 && ctx.dm ? ctx.dm->moleculeName(0) : "（无）";
    std::snprintf(buf, sizeof(buf), "FPS %.1f   分子 %zu   当前分子 %s", ctx.fps, count,
                  cur.c_str());
    ImGui::TextUnformatted(buf);
    ImGui::End();
}

} // namespace moleculens::app
