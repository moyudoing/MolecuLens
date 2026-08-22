// =============================================================================
// MolecuLens · app · events/event_bus.cpp —— 事件总线实现
// 规格来源：主方案 §9.2（P8：状态变更经事件总线广播；命令日志 JSON）
// =============================================================================

#include "app/events/event_bus.h"

#include <algorithm>

#include "spdlog/spdlog.h"

namespace moleculens::app {

void EventBus::publish(Event ev) {
    log_.push_back(ev);
    if (log_.size() > 1024) {
        log_.pop_front();
    }
    for (Subscriber* sub : subscribers_) {
        sub->onEvent(ev);
    }
    spdlog::debug("[app] event published: type={}", static_cast<int>(ev.type));
}

void EventBus::subscribe(Subscriber* sub) {
    if (std::find(subscribers_.begin(), subscribers_.end(), sub) ==
        subscribers_.end()) {
        subscribers_.push_back(sub);
    }
}

void EventBus::unsubscribe(Subscriber* sub) {
    subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), sub),
                       subscribers_.end());
}

void EventBus::poll() {
    for (Subscriber* sub : subscribers_) {
        sub->poll();
    }
}

} // namespace moleculens::app
