// =============================================================================
// MolecuLens · app · controller/timeline/timeline_controller.h —— 时间轴控制器
// 规格来源：主方案 §8.1/§8.2（FrameScheduler 播放状态机；帧变化 → FrameChanged
//           → DIRTY_COORDS；多分子活跃/静音）
// =============================================================================

#pragma once

#include <cstdint>

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/scheduler/frame_scheduler.h"

namespace moleculens::app {

class TimelineController : public Subscriber {
public:
    explicit TimelineController(AppServices& svc);

    // 播放状态机（主方案 §8.1）
    void setActive(uint32_t instanceIndex);
    void play();
    void pause();
    void stop();
    void toggleLoop(bool on);
    void seek(uint64_t frame);
    void step(int delta);
    void gotoFirst();
    void gotoLast();
    void setSpeed(float speed);
    void setDt(float dt);
    // 实时播放帧率（与物理 dt 解耦）。0 = 刷新同步；>0 = 固定帧率。
    // 这是旧入口：内部会把播放模式切到 FixedFps / RefreshSync。
    void setPlaybackFps(float fps);
    float playbackFps() const;

    // —— 播放节奏（v2.19 动态 FPS）——
    // 构造时从 settings.json 读一次并应用（启动即生效，不依赖 Workbench 先跑 UI）。
    void applyPlaybackSettings();
    void setPlaybackMode(engine::PlaybackMode mode);
    engine::PlaybackMode playbackMode() const;
    // Dynamic 的目标帧率上限；0 = 跟随渲染帧率。
    void setPlaybackTargetFps(float fps);
    float playbackTargetFps() const;
    // 每渲染帧最多推进几帧（1–8），卡顿后不积累 backlog。
    void setPlaybackMaxCatchUp(int frames);
    int playbackMaxCatchUp() const;
    // 最近一次推进的等效轨迹帧率（UI 显示"实际 58 FPS"）。
    float effectiveFps() const;
    float speed() const;
    uint64_t frame() const;
    uint64_t frameCount() const;
    float time() const;
    float dt() const;
    bool playing() const;
    bool loop() const;

    // —— 选区循环（In/Out，主方案 §8.2/§8.3）——
    // outFrame == 0 表示"到末帧"（UI 默认值就是 0，用户不必先知道总帧数）；
    // 控制器在 applyInOut() 里把 0 解析成 frameCount-1 再交给 FrameScheduler
    // （后者要求真实帧号）。帧数变化（setActive）后会自动重新应用。
    void setInOut(uint64_t inFrame, uint64_t outFrame);
    void setInOutEnabled(bool on);
    uint64_t inFrame() const { return inFrame_; }
    uint64_t outFrame() const;      // **实际生效**的末帧（0 → frameCount-1）
    bool inOutEnabled() const { return inOutEnabled_; }

    void poll(float dtReal) override;   // 每帧按**真实 dt** 推进（Subscriber::poll）
    void onEvent(const Event&) override;

private:
    void publishFrame();           // 写当前帧坐标 → CoordsSoA + 广播 FrameChanged
    void applyInOut();             // 把 In/Out（含 0=末帧语义）应用到 FrameScheduler
    AppServices& svc_;
    uint32_t activeInstance_ = 0;
    bool activeSet_ = false;
    uint64_t inFrame_ = 0;
    uint64_t outFrame_ = 0;        // 0 = 末帧
    bool inOutEnabled_ = false;
};

} // namespace moleculens::app
