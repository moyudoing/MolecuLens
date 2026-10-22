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

void FrameScheduler::setPlaybackMode(PlaybackMode mode) {
    if (mode == mode_) {
        return;
    }
    mode_ = mode;
    // 切模式时清零累积器：否则固定 1 FPS 剩下的半拍会在切到动态后立刻多走一帧，
    // 用户看到的是"切模式的瞬间跳帧"。
    accumulator_ = 0.0f;
}

void FrameScheduler::setDynamicFpsRange(float minFps, float maxFps) {
    minFps_ = std::clamp(minFps, 0.5f, 240.0f);
    maxFps_ = std::clamp(maxFps, minFps_, 480.0f);   // 保证 lo <= hi
}

float FrameScheduler::playbackInterval(float dt) const {
    // 没有渲染 dt 时退化成"本帧 dt"：保证首帧 / 手动推进也有正间隔。
    const float renderDt = smoothDt_ > 0.0f ? smoothDt_ : std::max(dt, 1e-4f);
    switch (mode_) {
    case PlaybackMode::FixedFps:
        return playbackFps_ > 0.0f ? 1.0f / std::max(playbackFps_, 0.1f)
                                   : std::max(renderDt, 1e-4f);
    case PlaybackMode::Dynamic: {
        // 目标帧率是"上限"：间隔不小于 1/目标帧率 —— 120Hz 渲染 + 目标 60 时
        // 每两个渲染帧推进一帧；目标 0 = 跟随渲染帧率（每个渲染帧一帧）。
        float base = targetFps_ > 0.0f ? std::max(renderDt, 1.0f / targetFps_) : renderDt;
        const float fastest = 1.0f / std::max(maxFps_, 1.0f);   // 间隔下限（最快）
        const float slowest = 1.0f / std::max(minFps_, 0.1f);   // 间隔上限（最慢）
        base = std::clamp(base, fastest, slowest);
        return std::max(base, 1e-4f);
    }
    case PlaybackMode::RefreshSync:
    default:
        return std::max(renderDt, 1e-4f);
    }
}

bool FrameScheduler::advance(float dtReal) {
    // 渲染 dt 的 EMA：所有模式都更新，切到 Dynamic 时立即有可用的平滑值。
    // 首帧直接取 dt（从 0 起步会把第一帧估成"极慢"）；alpha=0.2 约 10 帧稳定。
    if (dtReal > 0.0f) {
        smoothDt_ = smoothDt_ > 0.0f ? smoothDt_ + 0.2f * (dtReal - smoothDt_)
                                     : dtReal;
    }
    if (!playing_ || frameCount_ <= 1) {
        // 单帧场景没有"推进"语义：返回 false，避免时间轴每帧广播同一个帧号。
        advanced_ = false;
        return false;
    }
    advanced_ = false;
    // 实时播放间隔由当前模式决定；speed_ 是倍率（2× = 每秒推进两倍的帧）。
    // dtFrame_（物理时间步长）只用于时间显示与导出，绝不能参与这里的定时。
    const float dt = std::max(dtReal, 0.0f);
    const float interval = playbackInterval(dt);
    accumulator_ += dt * std::max(speed_, 0.0f);
    // Dynamic 有"每渲染帧最多 maxCatchUp 帧"的硬上限：卡顿 500ms 后只补几帧，
    // 多余的时间预算直接丢弃（不积累 backlog，不会突然快进一大段）。
    // 固定/同步模式沿用旧行为：最多 240 帧（一次卡顿最多补 4 秒@60FPS）。
    const int budget = mode_ == PlaybackMode::Dynamic ? maxCatchUp_ : 240;
    // eps 吸收浮点累减误差（1.0 s / (1/30) 恰好 30 帧，累减 30 次后可能差 1e-6）。
    const float eps = interval * 1e-3f;
    int steps = 0;
    while (accumulator_ + eps >= interval && interval > 0.0f && steps < budget) {
        accumulator_ -= interval;
        ++steps;
        step(1);
        if (!playing_) {
            break;   // 非循环播放到末帧：step 内部已暂停
        }
    }
    // 累积值不会无限增长：Dynamic 只保留不到一个间隔的余量（超出即丢弃），
    // 其余模式保留旧的 4 个间隔上限（低速播放时的合理余量）。
    accumulator_ = std::clamp(accumulator_, 0.0f,
                              mode_ == PlaybackMode::Dynamic ? interval : interval * 4.0f);
    // 等效轨迹帧率：间隔的倒数 × 速度倍率（Dynamic 下跟着渲染节奏走）。
    effectiveFps_ = interval > 0.0f ? std::min(speed_ / interval, 1000.0f) : 0.0f;
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
