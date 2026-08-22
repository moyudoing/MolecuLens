// =============================================================================
// MolecuLens · entity · trajectory/trajectory.h —— 轨迹容器
// 规格来源：主方案 §4.3 / §8.1（全内存模式；窗口模式预留；dt/periodic）
// =============================================================================

#pragma once

#include <memory>

#include "entity/scene/scene.h"
#include "entity/trajectory/timestep.h"

namespace moleculens::entity {

struct Trajectory {
    std::vector<Timestep> frames;        // 各帧坐标（共享拓扑）
    float dt = 2.0f;                     // 帧间隔（ps）
    bool  periodic = false;              // 周期盒子
    std::shared_ptr<class ITrajectoryReader> reader;  // 可选：惰性/流式数据源（§8.5）
};

} // namespace moleculens::entity
