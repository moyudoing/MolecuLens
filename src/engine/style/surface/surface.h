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

} // namespace moleculens::engine
