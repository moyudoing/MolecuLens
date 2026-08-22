// =============================================================================
// MolecuLens · entity · trajectory/timestep.h —— 单帧数据（SoA 扁平）
// 规格来源：主方案 §4.3（positions 单位 Å；可选 velocities/forces/box/energies）
// =============================================================================

#pragma once

#include <array>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::entity {

struct Timestep {
    std::vector<glm::vec3> positions;     // Å（加载时统一换算）
    std::vector<glm::vec3> velocities;    // 可选（GRO 速度字段）
    std::vector<glm::vec3> forces;        // 可选
    std::array<float, 9>   box{};         // 周期盒子（可选）
};

} // namespace moleculens::entity
