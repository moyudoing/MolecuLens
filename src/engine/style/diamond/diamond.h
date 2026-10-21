// =============================================================================
// MolecuLens · engine · style/diamond/diamond.h —— 菱形（八面体宝石）样式
// =============================================================================

#pragma once

#include "engine/style/base_style.h"

namespace moleculens::engine {

// 每个残基（单残基分子 = 每个实例）一颗八面体宝石。八面体用 TriMesh 发射，
// 面法线独立（flat shading）→ 宝石的棱面高光；颜色取该残基首原子配色，
// 并按面做 ±7% 亮度差，使立方体/球体之外的"菱形"轮廓一眼可辨。
void drawDiamond(const RepContext& ctx, SceneCommandList& out);

} // namespace moleculens::engine
