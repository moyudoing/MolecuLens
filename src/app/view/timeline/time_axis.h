// =============================================================================
// MolecuLens · app · view/timeline/time_axis.h —— 时间轴控件
// 规格来源：主方案 §8.3（TransportBar ⏮◀▶/⏸⏭ | 循环□ 选区循环□ | 速度；
//           Scrubber InvisibleButton 自绘轨道/刻度；帧信息）
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>

#include <imgui.h>

namespace moleculens::app {

class TimelineController;

class TimeAxis {
public:
    void setController(TimelineController* ctl) { ctl_ = ctl; }
    // 渲染整条时间轴（TransportBar + Scrubber + 帧信息）
    void render();

    bool showFps = true;
    // 帧率（由帧循环写入；showFps 打开时显示在帧信息行）。此前该字段声明后
    // 从未被任何调用方读写（死字段），现由 ViewControl 每帧写入。
    void setFps(float fps) { fps_ = fps; }

private:
    void renderTransportBar();
    void renderScrubber();
    // 帧信息文本（1 基帧号 + 时间 + dt + FPS），由 renderTransportBar 右对齐显示
    void frameInfoText(char* buf, size_t n) const;

    TimelineController* ctl_ = nullptr;
    float fps_ = 0.0f;
    // 选区循环勾选框状态镜像（ImGui::Checkbox 需要非 const bool*；真值以控制器为准，
    // 每帧同步，用户点击时才回写控制器）
    bool loopRange_ = false;
};

} // namespace moleculens::app
