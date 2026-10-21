// =============================================================================
// MolecuLens · io · cube/cube_volume.h —— Gaussian cube 体积数据（电子密度 / 静电势）
// 用途：ESP 映射表面 —— 密度 cube 定义等值面几何，ESP cube 决定逐顶点颜色。
// 约定：
//   ・解析后**长度单位统一为 Å**（cube 原始为 Bohr，已乘 0.529177210903）；
//   ・标量数据保持文件原单位：密度通常为 a.u.，ESP 通常为 Hartree；
//   ・数据索引布局 = (ix * ny + iy) * nz + iz（x 外层、z 最快）。
// 规格参考：Gaussian cube 文件格式（http://gaussian.com/cubegen/）+ Multiwfn 输出。
// =============================================================================

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::entity {

// 体积场种类（由文件标题/数值启发式判定；调用方也可显式覆盖）
enum class CubeFieldKind : uint8_t {
    Unknown = 0,
    ElectronDensity = 1,
    ElectrostaticPotential = 2,
};

// cube 文件头里的原子记录（Bohr → Å 已换算）
struct CubeAtom {
    int       atomicNumber = 0;      // Z（原子序数）
    float     nuclearCharge = 0.f;   // cube 第 2 列（一般等于 Z）
    glm::vec3 position{0.f};         // Å
};

// 解析后的体积数据
struct CubeVolume {
    std::array<int, 3>       dims{0, 0, 0};   // nx, ny, nz
    std::array<glm::vec3, 3> axes{};          // axes[i] = 沿第 i 轴每步的向量（Å）
    glm::vec3                origin{0.f};     // Å
    // 由 parseCubeFile 填充：axes 矩阵的逆（world Å → 分数体素坐标）
    glm::dmat3               axesInverse{1.0};

    std::vector<float>    data;    // size = nx*ny*nz；data[(ix*ny+iy)*nz+iz]
    std::vector<CubeAtom> atoms;
    CubeFieldKind         kind = CubeFieldKind::Unknown;

    std::size_t voxelCount() const {
        if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
            return 0;
        }
        return static_cast<std::size_t>(dims[0]) * static_cast<std::size_t>(dims[1]) *
               static_cast<std::size_t>(dims[2]);
    }

    bool valid() const {
        return dims[0] > 1 && dims[1] > 1 && dims[2] > 1 && data.size() == voxelCount();
    }

    float value(int ix, int iy, int iz) const {
        return data[(static_cast<std::size_t>(ix) * dims[1] + iy) * dims[2] + iz];
    }

    glm::vec3 voxelPosition(int ix, int iy, int iz) const {
        return origin + axes[0] * static_cast<float>(ix) +
               axes[1] * static_cast<float>(iy) + axes[2] * static_cast<float>(iz);
    }

    // 世界坐标（Å）三线性采样；出界返回 false（out 不变）
    bool sampleTrilinear(const glm::vec3& world, float& out) const;

    float minValue() const;
    float maxValue() const;
};

// 解析 Gaussian cube 文件（Bohr → Å；数据保持原单位）。失败返回 false 并写 error。
bool parseCubeFile(const std::string& path, CubeVolume& out, std::string* error = nullptr);

} // namespace moleculens::entity
