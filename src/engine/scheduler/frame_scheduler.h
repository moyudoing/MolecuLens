// =============================================================================
// MolecuLens · engine · scheduler/frame_scheduler.h —— 时间驱动播放（FrameScheduler）
// 规格来源：主方案 §8.2（frame/playing/loop/speed/inFrame/outFrame/accumulator；
//           时间驱动与渲染帧率解耦；帧变化由调用方广播）
// =============================================================================

#pragma once

#include <algorithm>
#include <cstdint>

namespace moleculens::engine {

// 播放节奏（v2.19）：
//   RefreshSync —— 每个渲染帧推进一帧（旧默认，最顺滑，但帧率越高播放越快）；
//   FixedFps    —— 固定播放帧率（12/20/24/30/60…，可降低 QuickSurf 预计算压力）；
//   Dynamic     —— 跟随真实渲染帧率：用渲染 dt 的 EMA 决定播放间隔，并夹在
//                  [1/maxFps, 1/minFps] 内，再受目标帧率上限与追帧上限约束。
//                  高刷屏不会把轨迹"加速播放"，掉帧时也不会一次性追几十帧。
enum class PlaybackMode : uint8_t { RefreshSync = 0, FixedFps = 1, Dynamic = 2 };

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
    // fps == 0 表示"跟随刷新率"：每个渲染帧推进一帧（默认的动画节奏，
    // 最顺滑；速度倍率仍然生效，例如 0.5× 就是每两个渲染帧推进一帧）。
    // 旧 API 语义保持不变：0 = 刷新同步，>0 = 固定帧率（现有调用点/测试依赖这一点）。
    // 若当前处于 Dynamic，这里会切回 FixedFps —— 动态模式的目标帧率请用
    // setPlaybackTargetFps()，它不会改变模式。
    void setPlaybackFps(float fps) {
        playbackFps_ = fps <= 0.0f ? 0.0f : std::clamp(fps, 1.0f, 240.0f);
        mode_ = playbackFps_ <= 0.0f ? PlaybackMode::RefreshSync : PlaybackMode::FixedFps;
        accumulator_ = 0.0f;   // 节奏变了，不继承旧节奏剩下的半拍
    }
    // 当前模式下的"配置帧率"：FixedFps 返回固定帧率，Dynamic 返回目标上限
    // （0 = 跟随渲染帧率），RefreshSync 返回 0。
    float playbackFps() const {
        return mode_ == PlaybackMode::Dynamic ? targetFps_ : playbackFps_;
    }
    bool refreshSynced() const { return mode_ == PlaybackMode::RefreshSync; }

    // —— 播放节奏（v2.19 动态 FPS）——
    void setPlaybackMode(PlaybackMode mode);
    PlaybackMode playbackMode() const { return mode_; }
    // Dynamic 的目标帧率上限；0 = 跟随渲染帧率（每个渲染帧最多一帧的间隔）。
    void setPlaybackTargetFps(float fps) {
        targetFps_ = fps <= 0.0f ? 0.0f : std::clamp(fps, 1.0f, 240.0f);
    }
    float playbackTargetFps() const { return targetFps_; }
    // Dynamic 的间隔夹逼范围：最快 1/maxFps、最慢 1/minFps（min ≤ max）。
    void setDynamicFpsRange(float minFps, float maxFps);
    float dynamicMinFps() const { return minFps_; }
    float dynamicMaxFps() const { return maxFps_; }
    // 每渲染帧最多推进的轨迹帧数（1–8）：卡顿后只补少量帧，多余 backlog 丢弃。
    void setMaxCatchUp(int frames) { maxCatchUp_ = std::clamp(frames, 1, 8); }
    int maxCatchUp() const { return maxCatchUp_; }
    // 最近一次推进的等效轨迹帧率（UI 显示"实际 58 FPS"用）。
    float effectiveFps() const { return effectiveFps_; }
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
    // 当前模式下的播放间隔（秒）：Dynamic 由渲染 dt 的平滑值 + 目标/追帧约束得出。
    float playbackInterval(float dt) const;

    uint64_t frame_ = 0;
    uint64_t frameCount_ = 0;
    bool playing_ = false;
    bool loop_ = true;
    float speed_ = 1.0f;
    float dtFrame_ = 2.0f;
    float playbackFps_ = 0.0f;   // FixedFps 的帧率；0 = 刷新同步（旧默认）
    PlaybackMode mode_ = PlaybackMode::RefreshSync;
    float targetFps_ = 0.0f;     // Dynamic 的目标上限；0 = 跟随渲染帧率
    float minFps_ = 10.0f;       // Dynamic 间隔上限 = 1/minFps
    float maxFps_ = 144.0f;      // Dynamic 间隔下限 = 1/maxFps
    int maxCatchUp_ = 2;         // 每渲染帧最多推进的轨迹帧数
    float smoothDt_ = 0.0f;      // 渲染 dt 的 EMA（alpha=0.2，首帧取 dt）
    float effectiveFps_ = 0.0f;  // 最近一次推进的等效轨迹帧率
    uint64_t inFrame_ = 0;
    uint64_t outFrame_ = 0;
    float accumulator_ = 0.0f;
    bool advanced_ = false;
};

} // namespace moleculens::engine
