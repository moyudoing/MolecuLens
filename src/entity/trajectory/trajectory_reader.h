// =============================================================================
// MolecuLens · entity · trajectory/trajectory_reader.h —— 轨迹读取协议
// 规格来源：主方案 §8.5（"trajectory reader" 插件接口）
// 约定：readFrame/nextFrame 返回的 Timestep 单位已换算为 Å
//
// 数据类型的唯一来源：`Timestep` / `Trajectory` 定义在 `entity/scene/scene.h`
// （全仓解析器与播放链路都使用它）。本目录曾各自重复定义过一份同名结构，
// 与 scene.h 冲突且从未被编译 —— 已删除，此处只保留读取接口。
//
// 实现方向：XTC / DCD / TRR 等二进制轨迹（chemfiles 或自研读取器）实现本接口，
// 由 io 层按扩展名分派；PDB 多帧已由 PdbParser 直接读入 Trajectory。
// =============================================================================

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "entity/result.h"
#include "entity/scene/scene.h"   // Timestep（唯一定义处）

namespace moleculens::entity {

class ITrajectoryReader {
public:
    virtual ~ITrajectoryReader() = default;

    virtual const char* name() const = 0;                    // "xtc" | "dcd" | "pdb-model"
    virtual bool canOpen(const std::filesystem::path&) const = 0;
    virtual Result<void> open(const std::filesystem::path&) = 0;
    virtual uint64_t frameCount() const = 0;

    // 原子数：调用方据此与目标结构做**拓扑匹配**（XTC/DCD 等二进制轨迹不含拓扑，
    // 必须与同源结构文件配对导入，否则坐标会错贴到别的结构上）。0 = 未知。
    [[nodiscard]] virtual uint32_t atomCount() const { return 0; }
    // 帧间隔（ps）：播放速度用。1.0 = 未知。
    [[nodiscard]] virtual float dt() const { return 1.0f; }

    virtual Result<Timestep> readFrame(uint64_t index) = 0;  // 随机访问
    virtual Result<Timestep> nextFrame() = 0;                // 顺序流式（可选）
    virtual void close() = 0;
};

} // namespace moleculens::entity
