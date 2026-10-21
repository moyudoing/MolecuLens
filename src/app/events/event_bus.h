// =============================================================================
// MolecuLens · app · events/event_bus.h —— 事件总线（发布-订阅）
// 规格来源：主方案 §9.2（publish 同步广播 + 写命令日志；subscribe 构造注册；
//           poll 主循环轮询 active Subscriber；log 回放栈）
// =============================================================================

#pragma once

#include <deque>
#include <vector>

#include "app/events/event.h"

namespace moleculens::app {

class Subscriber {
public:
    virtual ~Subscriber() = default;
    virtual void onEvent(const Event&) = 0;
    // dtReal = 距上一帧的真实秒数（帧循环实测）。时间驱动的订阅者（时间轴）
    // 必须用它推进——此前无参数、由时间轴硬编码 1/60，导致播放速度与真实
    // 帧率脱钩（帧率 30fps 时播放只有半速）。
    virtual void poll(float dtReal) { (void)dtReal; }
};

class EventBus {
public:
    void publish(Event ev);            // 同步广播 + 写命令日志
    void subscribe(Subscriber* sub);
    void unsubscribe(Subscriber* sub);
    void poll(float dtReal);           // 主循环轮询 active Subscriber（时间轴等）
    [[nodiscard]] const std::deque<Event>& log() const { return log_; }   // 回放栈

private:
    std::vector<Subscriber*> subscribers_;
    std::deque<Event> log_;            // 回放栈（撤销/脚本日志，上限 1024）
};

} // namespace moleculens::app
