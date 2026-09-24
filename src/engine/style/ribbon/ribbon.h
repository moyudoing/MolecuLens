// =============================================================================
// MolecuLens · engine · style/ribbon/ribbon.h —— Ribbon 渲染（丝带）
// 规格：薄带状 backbone 表现，仅显示骨架路径
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// Ribbon：backbone CA 原子薄带状表现
void drawRibbon(const RepContext& ctx, SceneCommandList& cmdList);

} // namespace moleculens::engine
