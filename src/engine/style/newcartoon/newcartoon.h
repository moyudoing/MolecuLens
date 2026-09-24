// =============================================================================
// MolecuLens · engine · style/newcartoon/newcartoon.h —— NewCartoon 渲染
// 规格：改进的卡通模式，更平滑的样条插值，更精细的二级结构表现
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// NewCartoon：改进卡通（更高采样率样条，更平滑的过渡）
void drawNewCartoon(const RepContext& ctx, SceneCommandList& cmdList);

} // namespace moleculens::engine
