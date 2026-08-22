// =============================================================================
// MolecuLens · app · controller/timeline/timeline_controller.h —— 时间轴控制器
// 规格来源：主方案 §8.1/§8.2（FrameScheduler 播放状态机；帧变化 → FrameChanged
//           → DIRTY_COORDS；多分子活跃/静音）
// =============================================================================

#pragma once

#include <cstdint>

#include "app/events/event_bus.h"
#include "app/shell/application.h"

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
    float speed() const;
    uint64_t frame() const;
    uint64_t frameCount() const;
    float time() const;
    float dt() const;
    bool playing() const;
    bool loop() const;

    void poll() override;          // 每帧推进（Subscriber::poll）
    void onEvent(const Event&) override;

private:
    void publishFrame();           // 写当前帧坐标 → CoordsSoA + 广播 FrameChanged
    AppServices& svc_;
    uint32_t activeInstance_ = 0;
    bool activeSet_ = false;
};

} // namespace moleculens::app
