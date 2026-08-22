// =============================================================================
// MolecuLens · engine · style/stick/stick.h —— 棍状（Stick）
// 规格来源：主方案 §5.7.3（键圆柱为主 cylR≈0.25；原子端帽小圆点可选）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

void drawStick(const RepContext&, SceneCommandList&);

} // namespace moleculens::engine
