// =============================================================================
// MolecuLens · entity · trajectory/trr/trr_reader.h —— TRR（GROMACS）轨迹读取器
//
// 格式来源：GROMACS TRR / xdrfile（Erik Lindahl & David van der Spoel，BSD-2）；
//           本实现按公开记录布局独立编写，坐标统一 nm → Å 返回。
//
// 与 XTC 的区别（DcdReader/XtcReader 的老手也容易混）：
//   · XTC 是**有损压缩**的坐标流（每帧 magic=1995，块压缩）；
//   · TRR 是**无压缩的 XDR 记录**（每帧 magic=1993），可携带坐标/速度/力，
//     体积通常大一个数量级（本仓库测试算例 md.trr 74 MB），但精度无损、字段齐全。
//
// 每帧记录布局（全部大端 XDR）：
//   int32 magic=1993, version, ir_size, e_size, box_size, vir_size, pres_size,
//         top_size, x_size, v_size, f_size, natoms, step, nre      （14 × 4 字节）
//   float32 t, lambda                                            （2 × 4 字节）
//   [box_size]  9 × float32 / float64（nm；36 或 72 字节）
//   [vir_size] [pres_size] [x_size] [v_size] [f_size]  依次为坐标/速度/力
//
// x_size = natoms*3*4（float 精度）或 natoms*3*8（double 精度），据此判定字段类型；
// 文件整体读入内存后建立帧偏移表，因此 readFrame 支持随机访问，nextFrame 顺序流式。
// =============================================================================

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "entity/trajectory/trajectory_reader.h"

namespace moleculens::entity {

class TrrReader : public ITrajectoryReader {
public:
    TrrReader() = default;
    ~TrrReader() override;

    [[nodiscard]] const char* name() const override { return "trr"; }
    [[nodiscard]] bool canOpen(const std::filesystem::path&) const override;
    Result<void> open(const std::filesystem::path&) override;
    [[nodiscard]] uint64_t frameCount() const override { return frameCount_; }
    Result<Timestep> readFrame(uint64_t index) override;   // 随机访问
    Result<Timestep> nextFrame() override;                 // 顺序流式
    void close() override;

    [[nodiscard]] uint32_t atomCount() const override { return atomCount_; }
    [[nodiscard]] float dt() const override { return dt_; }

private:
    std::vector<uint8_t> data_;          // 整文件读入（TRR 常见几十 MB 级）
    std::vector<size_t>  frameOffsets_;  // 每帧记录起始偏移
    uint32_t atomCount_ = 0;
    uint64_t frameCount_ = 0;
    uint64_t nextIndex_ = 0;
    float    dt_ = 1.0f;
    bool     open_ = false;
};

} // namespace moleculens::entity
