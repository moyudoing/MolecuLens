// =============================================================================
// MolecuLens · engine · style/label/label.h —— 标签（billboard 文字）
// 规格来源：主方案 §5.7.7（原子名/残基名/元素符号；字体图集 + billboard 四边形；
//           字符串池 appendString 统一驻留 UTF-8）
// =============================================================================

#pragma once

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"

namespace moleculens::engine {

// 预留：M2 落地（billboard + 字形图集）
void drawLabelPlaceholder(const RepContext&, SceneCommandList&);

} // namespace moleculens::engine
