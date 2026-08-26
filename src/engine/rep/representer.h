// =============================================================================
// MolecuLens · engine · rep/representer.h —— 表示生成器
// 规格来源：主方案 §5.2 / §5.8.2（每 (表示, 实例, 帧) 一个独立任务可并行；
//           kDrawTable 分派表；Selection 掩码 + 帧数据 → 命令段）
// =============================================================================

#pragma once

#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/rep/representation.h"

namespace moleculens::entity {
class SceneStructure;
struct MoleculeInstance;
struct FrameData;
} // namespace moleculens::entity

namespace moleculens::engine {

// 生成上下文（每任务一份）
struct RepContext {

    const entity::SceneStructure* scene = nullptr;

    const entity::MoleculeInstance* instance = nullptr;

    const entity::FrameData* frame = nullptr;
    const Representation* rep = nullptr;
    const std::vector<bool>* mask = nullptr;      // Selection 掩码（全局顺序）
    uint32_t instanceIndex = 0;
    uint32_t globalAtomBase = 0;                  // 实例首原子在全局段的位置
    float dt = 0.0f;
    uint64_t frameIndex = 0;
    uint8_t renderMode = 0;                       // 体系渲染模式（0实心/1线框/2透明/3点）
};

class Representer {
public:

    // 生成：rep × instance × frame → 命令流（准备可并行；独立命令段合并由调用方负责）
    void generate(const Representation& rep,
                  const entity::SceneStructure& scene,
                  const entity::MoleculeInstance& mi,
                  uint32_t instanceIndex,
                  const entity::FrameData& frame,
                  uint64_t frameIndex,
                  const std::vector<bool>& mask,
                  SceneCommandList& out);

    // 从 RepContext 取全局原子坐标（世界坐标）
    static const glm::vec3* globalPositions(const RepContext&);
    static size_t globalAtomCount(const RepContext&);
    // 配色（主方案 §5.2：元素 CPK / 链 / 残基 / BFactor / 统一）
    static glm::vec4 colorForAtom(const RepContext&, uint32_t globalAtomIndex);

private:
    using DrawFn = void (*)(const RepContext&, SceneCommandList&);
    static const DrawFn kDrawTable[static_cast<int>(RepresentationType::Count)];
};

} // namespace moleculens::engine
