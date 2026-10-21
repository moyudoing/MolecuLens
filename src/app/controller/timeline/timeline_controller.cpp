// =============================================================================
// MolecuLens · app · controller/timeline/timeline_controller.cpp
// 规格来源：主方案 §8.2（advance → 帧变化 → FrameChanged → DIRTY_COORDS；
//           帧切只重传坐标段 P4）
// =============================================================================

#include "app/controller/timeline/timeline_controller.h"

#include <algorithm>

#include "engine/scheduler/frame_scheduler.h"
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

TimelineController::TimelineController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

void TimelineController::setActive(uint32_t instanceIndex) {
    activeInstance_ = instanceIndex;
    activeSet_ = true;
    if (svc_.scene && svc_.scheduler) {
        const auto& inst = svc_.scene->instances();
        if (instanceIndex < inst.size() && inst[instanceIndex].trajectory) {
            svc_.scheduler->setFrameCount(inst[instanceIndex].trajectory->frames.size());
            svc_.scheduler->setDtFrame(inst[instanceIndex].trajectory->dt);
            // setFrameCount 会把 outFrame_ 重置为末帧 → 重新应用用户的选区
            applyInOut();
        } else {
            // 该实例没有轨迹（例如刚把轨迹卸载掉）：帧数回到 1，时间轴退回单帧，
            // 否则调度器会一直保留上一次的帧数，时间轴看着像"卸了还在"。
            svc_.scheduler->setFrameCount(1);
            svc_.scheduler->setDtFrame(0.0f);
            applyInOut();
            if (svc_.bus) {
                svc_.bus->publish({EventType::FrameChanged, 0});
            }
        }
    }
}

void TimelineController::applyInOut() {
    if (!svc_.scheduler) {
        return;
    }
    const uint64_t count = svc_.scheduler->frameCount();
    const uint64_t last = count > 0 ? count - 1 : 0;
    if (!inOutEnabled_) {
        svc_.scheduler->setInOut(0, last);   // 关：整段
        return;
    }
    const uint64_t in = std::min(inFrame_, last);
    // outFrame_ == 0 表示"到末帧"；否则夹到 [in, last]
    const uint64_t out = outFrame_ == 0 ? last : std::clamp(outFrame_, in, last);
    svc_.scheduler->setInOut(in, out);
}

void TimelineController::setInOut(uint64_t inFrame, uint64_t outFrame) {
    inFrame_ = inFrame;
    outFrame_ = outFrame;
    applyInOut();
}

void TimelineController::setInOutEnabled(bool on) {
    inOutEnabled_ = on;
    applyInOut();
}

uint64_t TimelineController::outFrame() const {
    const uint64_t count = frameCount();
    const uint64_t last = count > 0 ? count - 1 : 0;
    if (!inOutEnabled_) {
        return last;
    }
    return outFrame_ == 0 ? last : std::min(outFrame_, last);
}

void TimelineController::play() {
    if (svc_.scheduler) {
        svc_.scheduler->play();
    }
}

void TimelineController::pause() {
    if (svc_.scheduler) {
        svc_.scheduler->pause();
    }
}

void TimelineController::stop() {
    if (svc_.scheduler) {
        svc_.scheduler->stop();
        publishFrame();
    }
}

void TimelineController::toggleLoop(bool on) {
    if (svc_.scheduler) {
        svc_.scheduler->setLoop(on);
    }
}

void TimelineController::seek(uint64_t frame) {
    if (svc_.scheduler) {
        svc_.scheduler->seek(frame);
        publishFrame();
    }
}

void TimelineController::step(int delta) {
    if (svc_.scheduler) {
        svc_.scheduler->step(delta);
        publishFrame();
    }
}

void TimelineController::gotoFirst() {
    if (svc_.scheduler) {
        svc_.scheduler->gotoFirst();
        publishFrame();
    }
}

void TimelineController::gotoLast() {
    if (svc_.scheduler) {
        svc_.scheduler->gotoLast();
        publishFrame();
    }
}

void TimelineController::setSpeed(float speed) {
    if (svc_.scheduler) {
        svc_.scheduler->setSpeed(speed);
    }
}

void TimelineController::setDt(float dt) {
    if (svc_.scheduler) {
        svc_.scheduler->setDtFrame(dt);
    }
}

void TimelineController::setPlaybackFps(float fps) {
    if (svc_.scheduler) {
        svc_.scheduler->setPlaybackFps(fps);
    }
}

float TimelineController::playbackFps() const {
    return svc_.scheduler ? svc_.scheduler->playbackFps() : 30.0f;
}

float TimelineController::speed() const {
    return svc_.scheduler ? svc_.scheduler->speed() : 1.0f;
}

uint64_t TimelineController::frame() const {
    return svc_.scheduler ? svc_.scheduler->frame() : 0;
}

uint64_t TimelineController::frameCount() const {
    return svc_.scheduler ? svc_.scheduler->frameCount() : 0;
}

float TimelineController::time() const {
    return svc_.scheduler ? svc_.scheduler->time() : 0.0f;
}

float TimelineController::dt() const {
    return svc_.scheduler ? svc_.scheduler->dtFrame() : 2.0f;
}

bool TimelineController::playing() const {
    return svc_.scheduler ? svc_.scheduler->playing() : false;
}

bool TimelineController::loop() const {
    return svc_.scheduler ? svc_.scheduler->loop() : true;
}

void TimelineController::poll(float dtReal) {
    if (!svc_.scheduler) {
        return;
    }
    // 时间驱动（主方案 §8.2：与渲染帧率解耦）——用**帧循环实测的真实 dt**推进。
    // 此前硬编码 1/60：帧率 30fps 时每秒只被调 30 次，播放速度随之减半；
    // 反之高帧率下会加速。dt 上界防"卡顿一把跳几十帧"（切窗口/断点调试）。
    const float dt = std::clamp(dtReal, 0.0f, 0.1f);
    if (svc_.scheduler->advance(dt)) {
        publishFrame();
    }
}

void TimelineController::publishFrame() {
    if (!activeSet_ || !svc_.scene || !svc_.scheduler) {
        return;
    }
    const auto& inst = svc_.scene->instances();
    if (activeInstance_ >= inst.size() || !inst[activeInstance_].trajectory) {
        return;
    }
    const uint64_t f = svc_.scheduler->frame();
    // 帧坐标 → 世界坐标（乘实例位姿）的唯一实现在 SceneStructure::applyTrajectoryFrame
    // —— 动画导出走同一入口，避免该变换两处各写一份而漂移。
    svc_.scene->applyTrajectoryFrame(activeInstance_, f);
    if (svc_.bus) {
        svc_.bus->publish({EventType::FrameChanged, static_cast<int64_t>(f)});
    }
}

void TimelineController::onEvent(const Event& ev) {
    if (ev.type == EventType::SceneOpened) {
        // 自动激活首实例
        setActive(0);
    }
}

} // namespace moleculens::app
