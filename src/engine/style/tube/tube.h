// =============================================================================
// MolecuLens · engine · style/tube/tube.h —— Tube 渲染（管状）
// 规格：backbone 平滑管状，简化卡通，适合快速预览蛋白质结构
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// Tube：backbone CA 原子平滑管状（简化卡通，无二级结构区分）
void drawTube(const RepContext& ctx, SceneCommandList& cmdList);

} // namespace moleculens::engine
