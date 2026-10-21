// =============================================================================
// MolecuLens · engine · export/pov/pov_exporter.h —— POV-Ray 导出器
// 规格来源：主方案 §6.2（场景描述文件 .pov）
// =============================================================================

#pragma once

#include <filesystem>

#include "entity/result.h"
#include "engine/cmd/cmd.h"

namespace moleculens::engine {

class PovExporter {
public:
    // 命令流 → POV-Ray 场景描述（球 = sphere{}；圆柱 = cylinder{}；线 = 忽略或 cylinder）
    static entity::Result<void> exportScene(const SceneCommandList& list,
                                            const std::filesystem::path& povPath);
};

} // namespace moleculens::engine
