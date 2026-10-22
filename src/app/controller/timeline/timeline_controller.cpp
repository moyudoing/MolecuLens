// =============================================================================
// MolecuLens · app · controller/timeline/timeline_controller.cpp
// 规格来源：主方案 §8.2（advance → 帧变化 → FrameChanged → DIRTY_COORDS；
//           帧切只重传坐标段 P4）
// =============================================================================

#include "app/controller/timeline/timeline_controller.h"

#include <algorithm>
#include <chrono>

#include "engine/scheduler/frame_scheduler.h"
#include "entity/scene/scene.h"
#include "persist/settings/settings.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

TimelineController::TimelineController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
    // 启动即应用 settings.json 里的播放节奏（动态 FPS / 固定 / 同步 + 倍速 +
    // 追帧上限）——不依赖 Workbench 先把设置面板画一遍。
    applyPlaybackSettings();
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
    return svc_.scheduler ? svc_.scheduler->playbackFps() : 0.0f;
}

void TimelineController::applyPlaybackSettings() {
    if (!svc_.scheduler) {
        return;
    }
    // 读不到配置（首次启动/文件损坏）就用默认值：动态 FPS + 目标 60。
    persist::Settings settings;
    if (auto loaded = persist::SettingsStore::load(); loaded) {
        settings = *loaded;
    }
    auto* sch = svc_.scheduler;
    sch->setSpeed(std::clamp(settings.playbackSpeed, 0.0f, 64.0f));
    sch->setMaxCatchUp(settings.playbackMaxCatchUp);
    const float fps = static_cast<float>(std::clamp(settings.playbackFps, 0, 240));
    if (settings.playbackMode == "refresh") {
        sch->setPlaybackMode(engine::PlaybackMode::RefreshSync);
    } else if (settings.playbackMode == "fixed") {
        // 固定帧率不能是 0（0 = 刷新同步），坏配置退回 60 FPS。
        sch->setPlaybackFps(fps > 0.0f ? fps : 60.0f);
    } else {
        sch->setPlaybackMode(engine::PlaybackMode::Dynamic);
        sch->setPlaybackTargetFps(fps);
    }
}

void TimelineController::setPlaybackMode(engine::PlaybackMode mode) {
    if (svc_.scheduler) {
        svc_.scheduler->setPlaybackMode(mode);
    }
}

engine::PlaybackMode TimelineController::playbackMode() const {
    return svc_.scheduler ? svc_.scheduler->playbackMode()
                          : engine::PlaybackMode::RefreshSync;
}

void TimelineController::setPlaybackTargetFps(float fps) {
    if (svc_.scheduler) {
        svc_.scheduler->setPlaybackTargetFps(fps);
    }
}

float TimelineController::playbackTargetFps() const {
    return svc_.scheduler ? svc_.scheduler->playbackTargetFps() : 0.0f;
}

void TimelineController::setPlaybackMaxCatchUp(int frames) {
    if (svc_.scheduler) {
        svc_.scheduler->setMaxCatchUp(frames);
    }
}

int TimelineController::playbackMaxCatchUp() const {
    return svc_.scheduler ? svc_.scheduler->maxCatchUp() : 2;
}

float TimelineController::effectiveFps() const {
    return svc_.scheduler ? svc_.scheduler->effectiveFps() : 0.0f;
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
    const auto t0 = std::chrono::steady_clock::now();
    svc_.scene->applyTrajectoryFrame(activeInstance_, f);
    // 每 60 帧一条 debug：多实例体系（md.gro 2042 实例）的帧切开销可观，
    // 播放卡顿时先看这里，再看渲染器重放。
    static int sFramePerf = 0;
    if ((sFramePerf++ % 60) == 0) {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        if (ms > 2.0) {
            spdlog::debug("[perf] applyTrajectoryFrame {} -> {:.1f} ms", f, ms);
        }
    }
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
