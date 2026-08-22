// =============================================================================
// MolecuLens · engine · style/cartoon/cartoon.h —— 卡通（蛋白质风格，6 步）
// 规格来源：主方案 §5.7.5 / §5.7.9（骨架提取 → SS 判定 → Catmull-Rom 样条 →
//           平行传输标架 → 几何挤出 → 三角化 → TriMesh）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

void drawCartoon(const RepContext&, SceneCommandList&);

// 卡通常量（§5.7.9；可写入设置/场景文件按需覆盖）
struct CartoonConstants {
    float breakThreshold = 4.2f;       // CA 间距 > 4.2Å → 断链
    int   samplesPerSegment = 4;       // Catmull-Rom k
    int   tubeSegments = 8;            // 截面段数（管）
    float coilRadius = 0.35f;          // Coil 管半径
    float helixWidth = 1.6f;           // Helix Ribbon 宽
    float sheetStartWidth = 1.2f;      // Sheet 箭头渐变起点
    float sheetEndWidth = 2.4f;        // Sheet 箭头宽度

    // SS 判定（几何启发区间，§5.7.9）
    float phiHelixLo = -70.0f, phiHelixHi = -40.0f;
    float psiHelixLo = -50.0f, psiHelixHi = -20.0f;
    float phiSheetLo = -140.0f, phiSheetHi = -100.0f;
    float psiSheetLo = 100.0f, psiSheetHi = 150.0f;

    // SS 色（§5.7.9）
    float helixColor[3] = {1.00f, 0.25f, 0.25f};
    float sheetColor[3] = {1.00f, 0.85f, 0.20f};
    float coilColor[3]  = {0.35f, 0.85f, 0.40f};
};

// 二面角 φ/ψ 计算（N, CA, C 主链三原子）
bool calcPhiPsi(const glm::vec3& prevN, const glm::vec3& ca,
                const glm::vec3& c, const glm::vec3& nextN,
                float* outPhi, float* outPsi);

} // namespace moleculens::engine
