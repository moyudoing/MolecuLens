// =============================================================================
// MolecuLens v2 · app/workbench/analysis/molecule_stats.h —— 逐分子统计
//
// 分析面板与导出共用同一份统计结果，避免"界面显示一套、导出另一套"。
// 统计口径（写进界面/导出表头，避免歧义）：
//   · Rg      回转半径（质量加权，Å，首帧）
//   · RMSE    相对首帧的坐标均方根误差（未做叠合/最优旋转，Å）
//   · RMSF    每个原子相对自身时间平均位置的涨落的均方根（Å）
//   · 最大位移 任一原子在采样帧中的最大位移（Å）
// 无轨迹的分子：帧数 0，RMSE/RMSF/最大位移均为 0（只统计首帧几何）。
// 大轨迹（1001 帧 × 2000+ 分子）按采样上限抽帧，避免打开面板卡住主线程；
// 采样帧数写进统计结果，导出时如实标注。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::entity {
class SceneStructure;
struct MoleculeInstance;
} // namespace moleculens::entity

namespace moleculens::app {

struct MoleculeStat {
    uint32_t instance = 0;      // 实例下标（与分子列表一致）
    std::string name;           // 实例显示名
    std::string system;         // 所属体系名（空 = 未归组）
    size_t atoms = 0;
    size_t bonds = 0;
    uint64_t frames = 0;        // 文件里的总帧数（0 = 无轨迹）
    uint32_t framesUsed = 0;    // 实际参与 RMSE/RMSF 的采样帧数
    double mass = 0.0;          // 总质量（u）
    glm::vec3 centroid{0.0f};   // 首帧质心（世界坐标，Å）
    glm::vec3 extent{0.0f};     // 首帧包围盒尺寸（Å）
    float radiusOfGyration = 0.0f;   // 质量加权 Rg（Å）
    float rmse = 0.0f;               // 相对首帧 RMSE（Å，未叠合）
    float rmsf = 0.0f;               // 平均原子涨落（Å）
    float maxDisplacement = 0.0f;    // 最大单原子位移（Å）
};

struct MoleculeStatsSummary {
    size_t molecules = 0;
    size_t systems = 0;
    size_t withTrajectory = 0;
    size_t atoms = 0;
    size_t bonds = 0;
    uint64_t maxFrames = 0;
    double totalMass = 0.0;
    float meanAtoms = 0.0f;
    float meanBonds = 0.0f;
    float meanRadiusOfGyration = 0.0f;
    float maxRadiusOfGyration = 0.0f;
    float meanRmse = 0.0f;
    float maxRmse = 0.0f;
    std::string maxRmseName;         // 最大 RMSE 对应的分子名
    float meanRmsf = 0.0f;
};

struct MoleculeStats {
    std::vector<MoleculeStat> molecules;
    MoleculeStatsSummary summary;
};

// 计算整个场景的逐分子统计（首帧几何 + 轨迹涨落）。
// 纯只读：不修改场景、不依赖当前时间轴帧（始终以轨迹第 0 帧为参考）。
[[nodiscard]] MoleculeStats computeMoleculeStats(
    const entity::SceneStructure& scene);

// 导出用的统一表头/行（CSV 与 XLSX 共用；列含义见上）。
[[nodiscard]] std::vector<std::string> moleculeStatsCsvHeader();
[[nodiscard]] std::vector<std::vector<std::string>> moleculeStatsCsvRows(
    const MoleculeStats& stats);

} // namespace moleculens::app
