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
    if (!playing_ || frameCount_ <= 1) {
        // 单帧场景没有"推进"语义：返回 false，避免时间轴每帧广播同一个帧号。
        advanced_ = false;
        return false;
    }
    advanced_ = false;
    // 实时播放间隔 = 1 / 播放帧率；speed_ 是倍率（2× = 每秒推进 60 帧）。
    // dtFrame_（物理时间步长）只用于时间显示与导出，绝不能参与这里的定时。
    const float interval = 1.0f / std::max(playbackFps_, 0.1f);
    accumulator_ += std::max(dtReal, 0.0f) * std::max(speed_, 0.0f);
    // 上限保护：即使某帧卡顿 100 ms，也最多补 240 帧（避免一次性跑完整个轨迹）。
    // eps 吸收浮点累减误差（1.0 s / (1/30) 恰好 30 帧，累减 30 次后可能差 1e-6）。
    const float eps = interval * 1e-3f;
    int guard = 0;
    while (accumulator_ + eps >= interval && interval > 0.0f && guard++ < 240) {
        accumulator_ -= interval;
        step(1);
        if (!playing_) {
            break;   // 非循环播放到末帧：step 内部已暂停
        }
    }
    // 累积值不会无限增长：超过一帧预算仍未消化的部分保留，下一帧继续。
    accumulator_ = std::clamp(accumulator_, 0.0f, interval * 4.0f);
    return advanced_;
}

void FrameScheduler::seek(uint64_t frame) {
    frame_ = frameCount_ == 0 ? 0 : std::min(frame, frameCount_ - 1);
    accumulator_ = 0.0f;   // 手动定位后不要立刻被累积量再推一帧
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
