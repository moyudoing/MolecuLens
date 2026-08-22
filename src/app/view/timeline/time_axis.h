// =============================================================================
// MolecuLens · app · view/timeline/time_axis.h —— 时间轴控件
// 规格来源：主方案 §8.3（TransportBar ⏮◀▶/⏸⏭ | 循环□ 选区循环□ | 速度；
//           Scrubber InvisibleButton 自绘轨道/刻度；帧信息）
// =============================================================================

#pragma once

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

private:
    void renderTransportBar();
    void renderScrubber();
    void renderFrameInfo();

    TimelineController* ctl_ = nullptr;
};

} // namespace moleculens::app
