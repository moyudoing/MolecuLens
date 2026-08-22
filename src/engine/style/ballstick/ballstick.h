// =============================================================================
// MolecuLens · engine · style/ballstick/ballstick.h —— 球棍（CPK）
// 规格来源：主方案 §5.7.2（先球后棍保证深度正确；
//           r = clamp(0.25×vdW, 0.2, 0.6)×scale；键半径 0.15×scale；
//           computeEndpoints 保证"键不穿球、球不吞键"）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

void drawBallAndStick(const RepContext&, SceneCommandList&);

} // namespace moleculens::engine
