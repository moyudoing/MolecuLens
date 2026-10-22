// =============================================================================
// MolecuLens · engine · style/polyhedron/polyhedron.h —— 多面体（二十面体）样式
// =============================================================================

#pragma once

#include "engine/style/base_style.h"

namespace moleculens::engine {

// 每个**非氢原子**一颗二十面体（20 面 / 60 顶点），平面法线 + 每原子确定性随机
// 朝向 → 看起来像一颗颗切面珠子，与球/菱形形成明显区别。全氢结构回退为所有原子。
void drawPolyhedron(const RepContext& ctx, SceneCommandList& out);

} // namespace moleculens::engine
