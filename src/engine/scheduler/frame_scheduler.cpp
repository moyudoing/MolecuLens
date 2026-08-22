// =============================================================================
// MolecuLens · engine · scheduler/frame_scheduler.cpp —— 播放状态机实现
// 规格来源：主方案 §8.2（accumulator 累加；dtFrame 步进；选区循环 in/out）
// =============================================================================

#include "engine/scheduler/frame_scheduler.h"

#include <algorithm>

namespace moleculens::engine {

void FrameScheduler::setFrameCount(uint64_t count) {
    frameCount_ = count;
    frame_ = std::min(frame_, count > 0 ? count - 1 : 0);
    // outFrame_ 无条件跟随帧数（初值 0 时也必须更新）
    outFrame_ = count > 0 ? count - 1 : 0;
}

bool FrameScheduler::advance(float dtReal) {
    if (!playing_ || frameCount_ == 0) {
        advanced_ = false;
        return false;
    }
    advanced_ = false;
    accumulator_ += dtReal * speed_;
    while (accumulator_ >= dtFrame_ && dtFrame_ > 0.0f) {
        accumulator_ -= dtFrame_;
        step(1);
    }
    return advanced_;
}

void FrameScheduler::seek(uint64_t frame) {
    frame_ = frameCount_ == 0 ? 0 : std::min(frame, frameCount_ - 1);
    advanced_ = true;
}

void FrameScheduler::step(int delta) {
    if (frameCount_ == 0) {
        return;
    }
    int64_t next = static_cast<int64_t>(frame_) + delta;
    if (next > static_cast<int64_t>(outFrame_)) {
        if (loop_) {
            frame_ = inFrame_;
        } else {
            frame_ = outFrame_;
            playing_ = false;
        }
    } else if (next < static_cast<int64_t>(inFrame_)) {
        frame_ = inFrame_;
    } else {
        frame_ = static_cast<uint64_t>(next);
    }
    advanced_ = true;
}

void FrameScheduler::gotoFirst() { seek(inFrame_); }

void FrameScheduler::gotoLast() { seek(outFrame_); }

void FrameScheduler::setInOut(uint64_t inFrame, uint64_t outFrame) {
    inFrame_ = std::min(inFrame, outFrame);
    outFrame_ = outFrame;
    if (frameCount_ > 0) {
        outFrame_ = std::min(outFrame_, frameCount_ - 1);
    }
    if (frame_ < inFrame_ || frame_ > outFrame_) {
        frame_ = inFrame_;
    }
}

} // namespace moleculens::engine
