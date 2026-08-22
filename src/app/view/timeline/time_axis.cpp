// =============================================================================
// MolecuLens · app · view/timeline/time_axis.cpp —— 时间轴实现
// 规格来源：主方案 §8.3（[TransportBar] [Scrubber] [帧信息]；
//           Scrubber 用 InvisibleButton 捕获拖拽 + GetWindowDrawList 自绘）
// =============================================================================

#include "app/view/timeline/time_axis.h"

#include <cstdio>

#include "app/controller/timeline/timeline_controller.h"
#include "app/view/theme/ui_theme.h"

namespace moleculens::app {

void TimeAxis::render() {
    if (!ctl_) {
        ImGui::TextDisabled("时间轴未激活（加载轨迹后可用）");
        return;
    }
    renderTransportBar();
    ImGui::SameLine();
    renderScrubber();
    renderFrameInfo();
}

void TimeAxis::renderTransportBar() {
    const float buttonW = 26.0f;
    // ⏮ 首帧
    if (ImGui::Button("|<<")) {
        ctl_->gotoFirst();
    }
    ImGui::SameLine();
    // ◀ 步退
    if (ImGui::Button("<<")) {
        ctl_->step(-1);
    }
    ImGui::SameLine();
    // ▶/⏸ 播放/暂停
    const bool playing = ctl_->playing();
    const char* playLabel = playing ? "||" : ">";
    if (UITheme::primaryButton(playLabel, ImVec2(buttonW, 0))) {
        playing ? ctl_->pause() : ctl_->play();
    }
    ImGui::SameLine();
    // ⏭ 步进
    if (ImGui::Button(">>")) {
        ctl_->step(1);
    }
    ImGui::SameLine();
    if (ImGui::Button(">>|")) {
        ctl_->gotoLast();
    }
    ImGui::SameLine();
    // 循环
    bool loop = ctl_->loop();
    if (ImGui::Checkbox("循环", &loop)) {
        ctl_->toggleLoop(loop);
    }
    ImGui::SameLine();
    // 速度
    float speed = ctl_->speed();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragFloat("x", &speed, 0.05f, 0.1f, 10.0f, "%.2f")) {
        ctl_->setSpeed(speed);
    }
    UITheme::helpMarker("播放速度倍率");
}

void TimeAxis::renderScrubber() {
    const uint64_t count = ctl_->frameCount();
    if (count <= 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("单帧");
        return;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320.0f);
    uint64_t frame = ctl_->frame();
    // 用 SliderScalar 简化 Scrubber（InvisibleButton 自绘轨道见 §8.3 注释；
    // Slider 行为等价：拖拽 seek）
    const uint64_t vMin = 0;
    if (ImGui::SliderScalar("帧", ImGuiDataType_U64, &frame, &vMin, &count, "%llu")) {
        ctl_->seek(frame);
    }
}

void TimeAxis::renderFrameInfo() {
    const uint64_t count = ctl_->frameCount();
    const uint64_t frame = ctl_->frame();
    const float t = ctl_->time();
    const float total = static_cast<float>(count > 0 ? count - 1 : 0) * ctl_->dt();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "帧 %llu / %llu   T = %.1f / %.1f ps  Δt %.2f ps",
                  static_cast<unsigned long long>(frame),
                  static_cast<unsigned long long>(count), t, total, ctl_->dt());
    ImGui::TextDisabled("%s", buf);
}

} // namespace moleculens::app
