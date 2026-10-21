// =============================================================================
// MolecuLens · engine · export/obj/obj_exporter.h —— OBJ 导出器（.obj + .mtl）
// 规格来源：主方案 §6.2 / §12.4（遍历场景几何写文件）
// =============================================================================

#pragma once

#include <filesystem>
#include <string>

#include "entity/result.h"
#include "engine/cmd/cmd.h"

namespace moleculens::engine {

class ObjExporter {
public:
    // 将命令流中的几何（球/圆柱/线段/TriMesh）导出为 OBJ + MTL
    static entity::Result<void> exportScene(const SceneCommandList& list,
                                            const std::filesystem::path& objPath);
    static entity::Result<void> exportToStream(const SceneCommandList& list,
                                               std::ostream& outObj,
                                               std::ostream& outMtl);
};

} // namespace moleculens::engine
