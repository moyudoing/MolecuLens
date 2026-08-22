// =============================================================================
// MolecuLens · app · view/container/main_view.h —— 主区域容器
// 规格来源：主方案 §7.1/§7.7（RenderContext 指针传开关双向同步；renderAll 统一
//           渲染 "3D 视口 + 时间轴 + 状态栏"；3D 视口 ImGui::Begin → resizeViewport
//           → render → Image；IsItemHovered 在 Image() 之后判断命中才转发鼠标）
// =============================================================================

#pragma once

#include <memory>

#include <imgui.h>

namespace moleculens::app {

class DataManager;
class MolScene;
class TimeAxis;
class TimelineController;
class SelectionController;

struct RenderContext {
    bool* showTimeline = nullptr;     // 面板 × 按钮回写 ViewControl（双向同步）
    bool* showStatusBar = nullptr;
    DataManager* dm = nullptr;
    MolScene* scene = nullptr;
    TimeAxis* timeAxis = nullptr;
    TimelineController* timeline = nullptr;
    SelectionController* selection = nullptr;
    float fps = 0.0f;
};

class MainView {
public:
    ~MainView();

    // 统一渲染"3D 视口 + 时间轴 + 状态栏"（主方案 §7.1）
    void renderAll(RenderContext& ctx);

private:
    void renderViewport(RenderContext& ctx);
    void renderTimeline(RenderContext& ctx);
    void renderStatusBar(RenderContext& ctx);
};

} // namespace moleculens::app
