// =============================================================================
// MolecuLens · engine · style/surface/surface.h —— 表面（vdW 表面 / Marching Cubes）
// 规格来源：主方案 §5.7.6（vdW 表面 / SES；Marching Cubes；输出 TriMesh）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// vdW 表面：对原子 vdW 球并集做 Marching Cubes 抽取等值面 → TriMesh
void drawSurface(const RepContext&, SceneCommandList&);
// 体系级 vdW 表面（G5）：聚合全部可见实例原子（含溶剂）为一个整体表面 → TriMesh
void drawSystemSurface(const RepContext&, SceneCommandList&);

// 运动中冻结体系表面几何：有缓存则复用（不按坐标重算），暂停后由 Renderer 解冻
// 并重建。与 QuickSurf 同一语义，见 quicksurf.h。
void surfaceSetFreezeGeometry(bool frozen);
bool surfaceFreezeGeometry();

} // namespace moleculens::engine
