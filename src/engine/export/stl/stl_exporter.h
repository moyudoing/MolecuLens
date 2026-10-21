// =============================================================================
// MolecuLens · engine · export/stl/stl_exporter.h —— STL 导出器
// 规格来源：主方案 §6.2（三角网格 .stl；仅 TriMesh 几何）
// =============================================================================

#pragma once

#include <filesystem>

#include "entity/result.h"
#include "engine/cmd/cmd.h"

namespace moleculens::engine {

class StlExporter {
public:
    // 命令流中的 TriMesh 导出为二进制 STL
    static entity::Result<void> exportScene(const SceneCommandList& list,
                                            const std::filesystem::path& stlPath);
};

} // namespace moleculens::engine
