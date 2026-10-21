// =============================================================================
// MolecuLens · engine · style/dotcloud/dotcloud.h —— 点云样式
// =============================================================================

#pragma once

#include "engine/style/base_style.h"

namespace moleculens::engine {

// 每个原子一个"点精灵"（体系渲染模式临时切到 Point）。无键、无光照负载，
// 适合 10 万级原子的快速预览；颜色仍走当前配色方案。
void drawDotCloud(const RepContext& ctx, SceneCommandList& out);

} // namespace moleculens::engine
