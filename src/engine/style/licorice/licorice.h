// =============================================================================
// MolecuLens · engine · style/licorice/licorice.h —— Licorice 渲染（甘草棍）
// 规格：粗棍状，均匀半径圆柱（0.3Å），无端帽球，适合展示复杂键连接
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// Licorice：粗棍状（类似 Stick 但圆柱更粗，无端帽球）
void drawLicorice(const RepContext& ctx, SceneCommandList& cmdList);

} // namespace moleculens::engine
