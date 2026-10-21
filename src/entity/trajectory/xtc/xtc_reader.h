// =============================================================================
// Moleculens · entity · trajectory/xtc/xtc_reader.h —— XTC（GROMACS）轨迹读取器
// 格式来源：GROMACS XTC / xdrfile（Erik Lindahl & David van der Spoel，BSD-2）；
//           本实现按其公开格式独立编写（见 .cpp 顶部对算法的逐段说明）。
//
// 为什么自研而不是 vendor libxdrfile：
//   vcpkg 没有 xdrfile / gromacs port（实测查过端口目录），引入只能自行 vendor
//   一整套 C 源码 + 头；而本工程只需要**读**一种格式。解码算法本身约 200 行，
//   零依赖，且能用真实 GROMACS 产出物做验证（见 tests）。
//
// XTC 与 DCD 的差别（用 DcdReader 时容易踩）：
//   · 坐标单位是 **nm**，不是 Å —— 本读取器已换算为 Å 后返回（ITrajectoryReader
//     的约定是"返回的 Timestep 单位已换算为 Å"）。
//   · 每个原子名/元素都不在文件里，只有坐标 → 必须与同源结构文件（PDB/GRO）配对
//     导入，原子数不匹配即拒绝（由 IoController 校验）。
//   · 每帧自带 time（ps），dt 由首末帧时间差推算，而非 NSAVC×DELTA。
//   · 坐标经 3D 有损压缩（精度通常 0.001 nm = 0.01 Å），非逐位无损。
// =============================================================================

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "entity/trajectory/trajectory_reader.h"

namespace moleculens::entity {

class XtcReader : public ITrajectoryReader {
public:
    XtcReader() = default;
    ~XtcReader() override;

    [[nodiscard]] const char* name() const override { return "xtc"; }
    [[nodiscard]] bool canOpen(const std::filesystem::path&) const override;
    Result<void> open(const std::filesystem::path&) override;
    [[nodiscard]] uint64_t frameCount() const override { return frameCount_; }
    Result<Timestep> readFrame(uint64_t index) override;   // 随机访问
    Result<Timestep> nextFrame() override;                 // 顺序流式
    void close() override;

    [[nodiscard]] uint32_t atomCount() const override { return atomCount_; }
    [[nodiscard]] float dt() const override { return dt_; }

private:
    std::vector<uint8_t> data_;         // 整文件读入（轨迹文件通常数十 MB 级）
    std::vector<size_t>  frameOffsets_; // 每帧起始偏移（随机访问用）
    uint32_t atomCount_ = 0;
    uint64_t frameCount_ = 0;
    uint64_t nextIndex_ = 0;
    float    dt_ = 1.0f;
    bool     open_ = false;
};

} // namespace moleculens::entity
