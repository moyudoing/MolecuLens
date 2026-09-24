// =============================================================================
// MolecuLens · app · events/event.h —— 事件类型
// 规格来源：主方案 §9.1（EventType + Event{type, payload}）
//
// ⚠️ 各事件的**实际消费者现状**（2026-09 核对，改动时请同步更新）：
//   面板普遍采用"每帧读控制器真值"的模式，因此多数事件暂无订阅者。
//   新增消费者时请在此处登记，避免"发了没人收/收了没人发"的隐性死通道。
//
//   | 事件                  | 发布方                          | 消费者 |
//   |-----------------------|---------------------------------|--------|
//   | SceneOpened           | IoController::mergeParsed       | CameraController（自动取景）、TimelineController（激活首帧）、SceneController（日志） |
//   | SelectionChanged      | SelectionController::select/pick| MainView 状态栏（命中原子数） |
//   | 其余 5 类             | 各控制器                        | **暂无**（见下） |
//
//   无消费者的 5 类（MoleculeChanged / StructureChanged / RepresentationChanged /
//   FrameChanged / CameraChanged）保留作为**跨面板通知的扩展点**：它们都是
//   "状态已变更"的语义事件，未来插入订阅者不需要改动发布方。
//   若长期无人使用，应连同发布点一并删除，而不是让总线看起来在工作。
// =============================================================================

#pragma once

#include <cstdint>

namespace moleculens::app {

enum class EventType : uint8_t {
    SceneOpened,           // 场景加载完成        （消费者：取景/时间轴/日志）
    MoleculeChanged,       // 增删/显隐/拓扑      （暂无消费者）
    StructureChanged,      // 坐标/属性变更       （暂无消费者）
    RepresentationChanged, // 表示/配色变更       （暂无消费者）
    FrameChanged,          // 时间轴帧切换        （暂无消费者）
    SelectionChanged,      // 拾取/选择           （消费者：状态栏；payload = 命中原子数）
    CameraChanged,         // 相机变化            （暂无消费者；仅程序化变更广播，见 CameraController）
};

struct Event {
    EventType type = EventType::SceneOpened;
    int64_t payload = 0;
};

} // namespace moleculens::app
