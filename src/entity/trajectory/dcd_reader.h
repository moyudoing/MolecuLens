// =============================================================================
// MolecuLens · entity · trajectory/dcd_reader.h —— DCD（CHARMM/NAMD）轨迹读取器
// 格式来源：CHARMM DCD 二进制格式（NAMD / CHARMM 通用）
//
// 为什么自研而不是引 chemfiles：
//   chemfiles 0.10.4 的 CMakeLists 声明 `cmake_minimum_required(VERSION 2.8.12)`
//   （现代 CMake 需 policy 覆盖，CMake 4.x 直接拒绝），且**自带 molfile 插件
//   与内嵌 zlib/lzma/bzip2**（`$<TARGET_OBJECTS:molfiles>`，CHFL_SYSTEM_* 默认 OFF）
//   —— 对只需要的 XTC/DCD/TRR 而言是数百源文件的重型依赖，且 molfiles 的 MSVC
//   兼容性historically 不佳。DCD 无压缩、结构简单，自研约 200 行且零依赖。
//
// DCD 布局（little/big 由文件头判定，本实现按 little-endian；常见即如此）：
//   [0]   int32 = 84           记录长度标记
//   [4]   char[4] "CORD"       （坐标；"VELD" 为速度）
//   [8]   int32 NSET           帧数
//   [12]  int32 ISTART, [16] int32 NSAVC（帧间隔步数）
//   [84]  标题记录（int32 len + string + int32 len）
//         int32 NATOM（配对的 4/4 字节记录）
//         [可选] double DELTA（时间步长，ps）
//         每帧：int32 len + float32 X[NATOM] + int32 len
//               int32 len + float32 Y[NATOM] + int32 len
//               int32 len + float32 Z[NATOM] + int32 len
// =============================================================================

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "entity/trajectory/trajectory_reader.h"

namespace moleculens::entity {

class DcdReader : public ITrajectoryReader {
public:
    DcdReader() = default;
    ~DcdReader() override;

    [[nodiscard]] const char* name() const override { return "dcd"; }
    [[nodiscard]] bool canOpen(const std::filesystem::path&) const override;
    Result<void> open(const std::filesystem::path&) override;
    [[nodiscard]] uint64_t frameCount() const override { return frameCount_; }
    Result<Timestep> readFrame(uint64_t index) override;
    Result<Timestep> nextFrame() override;
    void close() override;

    // 帧间隔（ps）：由 NSAVC × DELTA 推算（缺 DELTA 时退回 1.0）
    [[nodiscard]] uint32_t atomCount() const override { return atomCount_; }
    [[nodiscard]] float dt() const override { return dt_; }

private:
    std::vector<uint8_t> data_;     // 整文件读入（轨迹文件通常数十 MB 级）
    size_t   framesBegin_ = 0;      // 首个坐标记录的起始偏移
    size_t   frameStride_ = 0;      // 相邻帧的字节跨度
    uint32_t atomCount_ = 0;
    uint64_t frameCount_ = 0;
    uint64_t nextIndex_ = 0;
    float    dt_ = 1.0f;
    bool     open_ = false;
};

} // namespace moleculens::entity
