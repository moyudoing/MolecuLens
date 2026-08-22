// =============================================================================
// MolecuLens · persist · mlscene/scene_serializer.h —— 场景序列化器
// 规格来源：主方案 §12.1/附录 G（MlSceneSerializer 读写接口）
// =============================================================================

#pragma once

#include "persist/mlscene/mlscene.h"

namespace moleculens::persist::mlscene {

// 便捷：从 SceneSnapshot 构建（entity 场景 ↔ 快照转换由调用方负责）
inline entity::Result<void> saveScene(const SceneSnapshot& snap,
                                      const std::filesystem::path& path,
                                      const SaveOptions& opts = {}) {
    return MlSceneSerializer::save(snap, path, opts);
}

} // namespace moleculens::persist::mlscene
