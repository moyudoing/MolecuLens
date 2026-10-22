// =============================================================================
// MolecuLens · app · view/timeline/time_axis.cpp —— 时间轴实现
// 布局（v2.11）：两行 ——
//   第 1 行：播放控制（|<< << ▶ >> >>|）＋ 循环 ＋ 速度，右端是帧信息
//   第 2 行：整宽拖动条（自绘轨道 + In/Out 高亮 + 播放头），启用 In/Out 时右端带
//           紧凑的「入 / 出」输入框。
// 之所以改成两行：旧版把控制、320px 拖动条、帧信息全塞在一行里，视口下方的窄条
// （52px）放不下 → 拖动条与帧信息被挤出可视区，看起来"时间轴坏了"。
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/timeline/time_axis.h"

#include <algorithm>
#include <cstdio>

#include "app/controller/timeline/timeline_controller.h"
#include "app/view/theme/ui_theme.h"

namespace moleculens::app {

void TimeAxis::render() {
    if (!ctl_) {
        ImGui::TextDisabled(tr("时间轴未激活（加载轨迹后可用）"));
        return;
    }
    const bool hasTrajectory = ctl_->frameCount() > 1;
    ImGui::BeginDisabled(!hasTrajectory);
    renderTransportBar();
    ImGui::EndDisabled();
    if (!hasTrajectory) {
        ImGui::TextDisabled("%s", tr("（无轨迹：单帧）"));
        return;
    }
    renderScrubber();
}

// 第 1 行：控制按钮 + 循环 + 速度；右端右对齐帧信息。
void TimeAxis::renderTransportBar() {
    const float buttonW = 26.0f;
    if (ImGui::Button("|<<")) {
        ctl_->gotoFirst();
    }
    ImGui::SameLine();
    if (ImGui::Button("<<")) {
        ctl_->step(-1);
    }
    ImGui::SameLine();
    const bool playing = ctl_->playing();
    if (UITheme::primaryButton(playing ? "||" : ">", ImVec2(buttonW, 0))) {
        playing ? ctl_->pause() : ctl_->play();
    }
    ImGui::SameLine();
    if (ImGui::Button(">>")) {
        ctl_->step(1);
    }
    ImGui::SameLine();
    if (ImGui::Button(">>|")) {
        ctl_->gotoLast();
    }
    ImGui::SameLine();
    bool loop = ctl_->loop();
    if (ImGui::Checkbox(tr("循环"), &loop)) {
        ctl_->toggleLoop(loop);
    }
    ImGui::SameLine();
    float speed = ctl_->speed();
    ImGui::SetNextItemWidth(72.0f);
    if (ImGui::DragFloat("##speed", &speed, 0.05f, 0.1f, 10.0f, "%.2f×")) {
        ctl_->setSpeed(speed);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("播放速度倍率（× 播放帧率；与轨迹物理 dt 无关）"));
    }

    // 帧信息右对齐（同一行）
    char buf[192];
    frameInfoText(buf, sizeof(buf));
    const float tw = ImGui::CalcTextSize(buf).x;
    const float x = ImGui::GetWindowWidth() - tw - 14.0f;
    if (x > ImGui::GetCursorPosX() + 8.0f) {
        ImGui::SameLine(x);
    } else {
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%s", buf);
}

// 第 2 行：整宽拖动条；启用 In/Out 时右端是紧凑输入框。
void TimeAxis::renderScrubber() {
    const uint64_t count = ctl_->frameCount();
    const bool inOutOn = ctl_->inOutEnabled();
    const float inOutW = inOutOn ? 208.0f : 0.0f;
    const float width = std::max(ImGui::GetContentRegionAvail().x - inOutW, 80.0f);
    const float height = ImGui::GetFrameHeight() + 4.0f;   // 加大命中区（细条难拖）
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##scrubber", ImVec2(width, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    // 滚轮逐帧（时间轴自己的"滚动"）。拖条本身对滚轮无响应，而视口缩放也
    // 需要滚轮 —— 两者命中区不重叠，互不抢占。
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        ctl_->step(ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y0 = origin.y + 3.0f;
    const float y1 = origin.y + height - 3.0f;
    const float yMid = 0.5f * (y0 + y1);
    const uint64_t last = count - 1;

    dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(origin.x + width, y1),
                      ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

    // 选区循环区间高亮
    const uint64_t inF = inOutOn ? std::min(ctl_->inFrame(), last) : 0;
    const uint64_t outF = inOutOn ? std::min(ctl_->outFrame(), last) : last;
    if (inOutOn && outF > inF) {
        const float fx0 = origin.x + width * (static_cast<float>(inF) / static_cast<float>(last));
        const float fx1 = origin.x + width * (static_cast<float>(outF) / static_cast<float>(last));
        dl->AddRectFilled(ImVec2(fx0, y0), ImVec2(fx1, y1),
                          ImGui::GetColorU32(ImGuiCol_ButtonActive), 3.0f);
    }

    // 按下即跳（不必先移动鼠标）；按住拖动持续定位。命中区用整行高度，
    // 用户点轨道上下几像素也能抓到。
    if (active) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float t = std::clamp((mx - origin.x) / width, 0.0f, 1.0f);
        ctl_->seek(static_cast<uint64_t>(t * static_cast<float>(last) + 0.5f));
    } else if (hovered) {
        dl->AddRect(ImVec2(origin.x, origin.y + 1.0f),
                    ImVec2(origin.x + width, origin.y + height - 1.0f),
                    ImGui::GetColorU32(ImGuiCol_Text, 0.22f), 3.0f);
    }

    // 播放头
    const uint64_t frame = ctl_->frame();
    const float px = origin.x + width * (static_cast<float>(frame) / static_cast<float>(last));
    dl->AddRectFilled(ImVec2(px - 2.0f, y0), ImVec2(px + 2.0f, y1),
                      ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 2.0f);
    dl->AddCircleFilled(ImVec2(px, yMid), 5.0f, ImGui::GetColorU32(ImGuiCol_SliderGrab));

    if (!inOutOn) {
        return;
    }
    // In/Out 紧凑输入（1 基显示；出 = 0 表示末帧，这里显示为实际末帧）
    ImGui::SameLine();
    loopRange_ = inOutOn;
    if (ImGui::Checkbox(tr("选区"), &loopRange_)) {
        ctl_->setInOutEnabled(loopRange_);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    int inV = static_cast<int>(std::min(ctl_->inFrame(), last));
    if (ImGui::DragInt(tr("入"), &inV, 0.3f, 0, static_cast<int>(last))) {
        ctl_->setInOut(static_cast<uint64_t>(std::clamp(inV, 0, static_cast<int>(last))),
                       ctl_->outFrame());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    int outV = static_cast<int>(outF);
    if (ImGui::DragInt(tr("出"), &outV, 0.3f, 0, static_cast<int>(last))) {
        ctl_->setInOut(ctl_->inFrame(),
                       static_cast<uint64_t>(std::clamp(outV, 0, static_cast<int>(last))));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("在 [入, 出] 区间内循环播放"));
    }
}

// 帧信息文本（1 基帧号，与「轨迹」面板一致）
void TimeAxis::frameInfoText(char* buf, size_t n) const {
    const uint64_t count = ctl_->frameCount();
    const uint64_t frame = ctl_->frame();
    const float t = ctl_->time();
    const float total = static_cast<float>(count > 0 ? count - 1 : 0) * ctl_->dt();
    char fps[48];
    fps[0] = '\0';
    if (showFps && fps_ > 0.0f) {
        std::snprintf(fps, sizeof(fps), "   %.0f FPS", static_cast<double>(fps_));
    }
    // 用 "dt" 而非 "Δt"：旧默认字体（OPPO_B）的 cmap 里没有 U+0394（希腊字母 Δ）；
    // 现字体（Noto Sans SC 子集）已覆盖 Δ，文案保持不变。
    std::snprintf(buf, n, "%s %llu / %llu   T %.1f / %.1f ps   dt %.2f ps   %.0f FPS%s",
                  tr("帧"),
                  static_cast<unsigned long long>(count > 0 ? frame + 1 : 0),
                  static_cast<unsigned long long>(count), t, total, ctl_->dt(),
                  static_cast<double>(ctl_->playbackFps()), fps);
}

} // namespace moleculens::app
