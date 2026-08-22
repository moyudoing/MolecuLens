// =============================================================================
// MolecuLens · engine · style/surface/surface.h —— 表面（预留 P2）
// 规格来源：主方案 §5.7.6（vdW 表面 / SES；Marching Cubes 或 GPGPU；
//           输出 TriMesh；管线预留 surface 管槽与材质接口）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// 预留：P2 落地（Marching Cubes）
void drawSurfacePlaceholder(const RepContext&, SceneCommandList&);

} // namespace moleculens::engine
