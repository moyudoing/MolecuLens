// =============================================================================
// MolecuLens · entity · trajectory/trajectory_reader.h —— 轨迹读取协议
// 规格来源：主方案 §8.5（"trajectory reader" 插件接口；首期内建 pdb-model）
// 约定：readFrame/nextFrame 返回的 Timestep 单位已换算为 Å
// =============================================================================

#pragma once

#include <filesystem>
#include <string>

#include "entity/result.h"
#include "entity/trajectory/timestep.h"

namespace moleculens::entity {

class ITrajectoryReader {
public:
    virtual ~ITrajectoryReader() = default;

    virtual const char* name() const = 0;                    // "xtc" | "dcd" | "pdb-model"
    virtual bool canOpen(const std::filesystem::path&) const = 0;
    virtual Result<void> open(const std::filesystem::path&) = 0;
    virtual uint64_t frameCount() const = 0;
    virtual Result<Timestep> readFrame(uint64_t index) = 0;  // 随机访问
    virtual Result<Timestep> nextFrame() = 0;                // 顺序流式（可选）
    virtual void close() = 0;
};

} // namespace moleculens::entity
