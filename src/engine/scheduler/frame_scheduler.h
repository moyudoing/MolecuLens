// =============================================================================
// MolecuLens · engine · scheduler/frame_scheduler.h —— 时间驱动播放（FrameScheduler）
// 规格来源：主方案 §8.2（frame/playing/loop/speed/inFrame/outFrame/accumulator；
//           时间驱动与渲染帧率解耦；帧变化由调用方广播）
// =============================================================================

#pragma once

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
    void setSpeed(float speed) { speed_ = speed; }
    float speed() const { return speed_; }
    void setDtFrame(float dt) { dtFrame_ = dt; }
    float dtFrame() const { return dtFrame_; }
    void play() { playing_ = true; }
    void pause() { playing_ = false; }
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
    uint64_t inFrame_ = 0;
    uint64_t outFrame_ = 0;
    float accumulator_ = 0.0f;
    bool advanced_ = false;
};

} // namespace moleculens::engine
