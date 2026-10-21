// =============================================================================
// MolecuLens · engine · style/plate/plate.h —— Plate 渲染（平板 / 示意辅基）
// 规格：每个残基（或每个分子实例）一块扁平方板，用主成分轴定朝向。
// 用途：示意图里把叶绿素/辅基/配体画成参考图那样的彩色小方片，而不是球棍。
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// Plate：按残基/链分组的扁平方板（TriMesh 发射，走既有网格管线）
void drawPlate(const RepContext& ctx, SceneCommandList& cmdList);

} // namespace moleculens::engine
