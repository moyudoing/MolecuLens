// =============================================================================
// MolecuLens · app · events/event.h —— 事件类型（8 类）
// 规格来源：主方案 §9.1（EventType + Event{type, payload}）
// =============================================================================

#pragma once

#include <cstdint>

namespace moleculens::app {

enum class EventType : uint8_t {
    SceneOpened,           // 场景加载完成
    MoleculeChanged,       // 增删/显隐/拓扑
    StructureChanged,      // 坐标/属性变更
    RepresentationChanged, // 表示/配色变更
    FrameChanged,          // 时间轴帧切换
    SelectionChanged,      // 拾取/选择
    CameraChanged,         // 相机变化
    ScriptRequest,         // Python/RPC → 控制器
};

struct Event {
    EventType type = EventType::SceneOpened;
    int64_t payload = 0;
};

} // namespace moleculens::app
