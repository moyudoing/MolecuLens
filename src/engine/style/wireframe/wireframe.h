// =============================================================================
// MolecuLens · engine · style/wireframe/wireframe.h —— 线状（Wireframe）
// 规格来源：主方案 §5.7.4（键画线段；超大体系百万原子快速预览）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

void drawWireframe(const RepContext&, SceneCommandList&);

} // namespace moleculens::engine
