// =============================================================================
// MolecuLens · engine · style/spacefill/spacefill.h —— 空间填充（VdW）
// 规格来源：主方案 §5.7.1（每原子一个球，半径 = vdW × 缩放；纯实例化单 Draw Call；
//           单位 icosphere 细分 2 级 80 三角形；片元解析法线）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// kDrawTable 分派入口（实现见 spacefill.cpp）
void drawSpacefill(const RepContext&, SceneCommandList&);
// VDW：范德华全半径空间填充（不受 radiusScale 缩放，区别于 CPK/Spacefill）
void drawVdw(const RepContext&, SceneCommandList&);

} // namespace moleculens::engine
