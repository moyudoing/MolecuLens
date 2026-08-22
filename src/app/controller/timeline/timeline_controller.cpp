// =============================================================================
// MolecuLens · app · controller/timeline/timeline_controller.cpp
// 规格来源：主方案 §8.2（advance → 帧变化 → FrameChanged → DIRTY_COORDS；
//           帧切只重传坐标段 P4）
// =============================================================================

#include "app/controller/timeline/timeline_controller.h"

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
        }
    }
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

void TimelineController::poll() {
    if (!svc_.scheduler) {
        return;
    }
    // 时间驱动（主方案 §8.2：与渲染帧率解耦）
    if (svc_.scheduler->advance(1.0f / 60.0f)) {
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
    const auto& traj = inst[activeInstance_].trajectory;
    const uint64_t f = svc_.scheduler->frame();
    if (f < traj->frames.size()) {
        // 写当前帧坐标 → CoordsSoA（变换 + 帧坐标；帧切只重传坐标段 P4）
        const auto& frame = traj->frames[f];
        std::vector<glm::vec3> world;
        world.reserve(frame.positions.size());
        const glm::mat4 m = inst[activeInstance_].transform.matrix();
        for (const auto& p : frame.positions) {
            world.push_back(glm::vec3(m * glm::vec4(p, 1.0f)));
        }
        svc_.scene->writeFramePositions(activeInstance_, world);
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
