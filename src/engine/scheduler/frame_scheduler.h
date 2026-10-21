// =============================================================================
// MolecuLens · engine · scheduler/frame_scheduler.h —— 时间驱动播放（FrameScheduler）
// 规格来源：主方案 §8.2（frame/playing/loop/speed/inFrame/outFrame/accumulator；
//           时间驱动与渲染帧率解耦；帧变化由调用方广播）
// =============================================================================

#pragma once

#include <algorithm>
#include <cstdint>

namespace moleculens::engine {

class FrameScheduler {
public:
    FrameScheduler() = default;

    void setFrameCount(uint64_t count);
    uint64_t frameCount() const { return frameCount_; }

    // 每帧推进（dtReal 秒）；返回本调用内是否发生了帧变化
    bool advance(float dtReal);
    void seek(uint64_t frame);
    void step(int delta);
    void gotoFirst();
    void gotoLast();
    void setLoop(bool on) { loop_ = on; }
    bool loop() const { return loop_; }
    void setInOut(uint64_t inFrame, uint64_t outFrame);
    void setSpeed(float speed) { speed_ = std::clamp(speed, 0.0f, 64.0f); }
    float speed() const { return speed_; }
    void setDtFrame(float dt) { dtFrame_ = dt; }
    float dtFrame() const { return dtFrame_; }
    // 播放帧率（实时动画每秒推进多少轨迹帧）。**与物理 dt 无关**：
    // dtFrame 是轨迹文件的时间步长（10 ps 之类），只用于显示时间/导出时间轴；
    // 若拿它当实时步进间隔，10 ps 会变成"每 10 秒一帧"——这就是用户反馈的
    // "不能自动播放"。速度倍率作用在这个帧率上。
    void setPlaybackFps(float fps) { playbackFps_ = std::clamp(fps, 1.0f, 240.0f); }
    float playbackFps() const { return playbackFps_; }
    void play() {
        if (!playing_) {
            accumulator_ = 0.0f;   // 暂停后重新播放不补帧（避免一次性跳很多帧）
        }
        playing_ = true;
    }
    void pause() { playing_ = false; accumulator_ = 0.0f; }
    void stop() { playing_ = false; seek(0); }
    bool playing() const { return playing_; }
    uint64_t frame() const { return frame_; }
    float time() const { return static_cast<float>(frame_) * dtFrame_; }

private:
    uint64_t frame_ = 0;
    uint64_t frameCount_ = 0;
    bool playing_ = false;
    bool loop_ = true;
    float speed_ = 1.0f;
    float dtFrame_ = 2.0f;
    float playbackFps_ = 30.0f;   // 实时播放帧率（与物理 dt 解耦）
    uint64_t inFrame_ = 0;
    uint64_t outFrame_ = 0;
    float accumulator_ = 0.0f;
    bool advanced_ = false;
};

} // namespace moleculens::engine
