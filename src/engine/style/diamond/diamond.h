// =============================================================================
// MolecuLens · engine · style/diamond/diamond.h —— 菱形（八面体宝石）样式
// =============================================================================

#pragma once

#include "engine/style/base_style.h"

namespace moleculens::engine {

// 每个**非氢原子**一颗八面体宝石（v2.12 修订：不再以分子/残基为单位）。
// 八面体用 TriMesh 发射，面法线独立（flat shading）→ 宝石的棱面高光；颜色取
// 该原子配色，并按面做 ±7% 亮度差，使"菱形"轮廓一眼可辨。全氢结构回退为所有
// 选中原子，避免空白。
void drawDiamond(const RepContext& ctx, SceneCommandList& out);

} // namespace moleculens::engine
