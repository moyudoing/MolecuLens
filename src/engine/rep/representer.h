// =============================================================================
// MolecuLens · engine · rep/representer.h —— 表示生成器
// 规格来源：主方案 §5.2 / §5.8.2（每 (表示, 实例, 帧) 一个独立任务可并行；
//           分派表收敛到 engine/style/style_registry.h —— 单一事实来源；
//           Selection 掩码 + 帧数据 → 命令段）
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
    // 实例级类型范围掩码（v2.19）：与 instances() 一一对应，0 = 该实例被范围
    // 过滤掉、不参与本表示的渲染。nullptr = 不过滤（旧行为）。整体系表面
    // （surface/quicksurf 的 systemSurface 路径）也必须消费它，否则“只渲染
    // 一半溶剂”的整体系表面仍然包含全部溶剂。
    const std::vector<uint8_t>* instanceMask = nullptr;
    uint32_t instanceIndex = 0;
    uint32_t globalAtomBase = 0;                  // 实例首原子在全局段的位置
    float dt = 0.0f;
    uint64_t frameIndex = 0;
    uint8_t renderMode = 0;                       // 体系渲染模式（0实心/1线框/2透明/3点）
    // true = 坐标重放路径：命令序列（含 RenderMode/MaterialBlock 等状态命令）已
    // 存在于旧命令流，样式此时**只能发射坐标类命令**，否则重放游标失配。样式若
    // 平时会临时改 RenderMode（如点云），重放时必须跳过该状态命令。
    bool replay = false;
};

class Representer {
public:
    // 生成：rep × instance × frame → 命令流（准备可并行；独立命令段合并由调用方负责）
    // 样式分派经 engine/style/style_registry.h 的 styleFor()（先运行时覆盖层、
    // 后编译期静态表）——统一注册机制，热路径与插件扩展共用同一入口。
    // globalAtomBaseOverride：调用方若已有一份"逐实例起始原子下标"的前缀和，
    // 直接传入可避免每实例一次 O(n) 的 globalAtomBase 扫描（大体系渲染时是
    // O(n²) 热点）。UINT32_MAX = 未提供，内部按场景计算。
    void generate(const Representation& rep,
                  const entity::SceneStructure& scene,
                  const entity::MoleculeInstance& mi,
                  uint32_t instanceIndex,
                  const entity::FrameData& frame,
                  uint64_t frameIndex,
                  const std::vector<bool>& mask,
                  SceneCommandList& out,
                  uint32_t globalAtomBaseOverride = UINT32_MAX);

    // 从 RepContext 取全局原子坐标（世界坐标）
    static const glm::vec3* globalPositions(const RepContext&);
    static size_t globalAtomCount(const RepContext&);
    // 配色（主方案 §5.2：元素 CPK / 链 / 残基 / BFactor / 统一）
    static glm::vec4 colorForAtom(const RepContext&, uint32_t globalAtomIndex);
    // 不依赖 ctx.instance 的取色入口（v2.19）：整体系表面的 RepContext 没有
    // 单一 instance（它遍历整个体系的所有实例），旧实现直接读 ctx.instance →
    // 空指针解引用崩溃。structureIndex/localAtomIndex 由调用方按该原子真实所属
    // 实例给出；instanceIndex 只用于 Chain/Residue 的实例抖动（保证同结构多实例
    // 颜色不同）。
    static glm::vec4 colorForAtomAt(const RepContext&, uint32_t structureIndex,
                                    uint32_t localAtomIndex, uint32_t instanceIndex);

    // 浅色背景（如 --bg white 导出 PNG）时，把"按原子名"里近似白色的氢压暗，
    // 否则白底白线等于看不见。不影响深色背景的常规显示。
    static void setLightBackground(bool on);

private:
    static bool lightBackground_;   // 浅色背景导出时压暗氢，避免白底白线
    // 原始取色（按 sRGB 语义书写的调色板值）；公开的 colorForAtom 会转成线性。
    static glm::vec4 colorForAtomSrgb(const RepContext&, uint32_t globalAtomIndex);
    // 取色核心（sRGB 语义）：只依赖显式传入的 (结构, 实例内原子, 实例) 三元组，
    // **绝不读 ctx.instance** —— 整体系表面的 RepContext 没有单一实例。
    static glm::vec4 colorForAtomAtSrgb(const RepContext&, uint32_t structureIndex,
                                        uint32_t localAtomIndex, uint32_t instanceIndex);
};

// 实例级类型范围掩码（v2.19）。输出与 instances 一一对应：1 = 参与本表示渲染。
// 语义见 Representation::rangeMode 的注释（类型 + 比例/步长，确定性顺序）。
// 声明放在表示层头（避免 representation.h 反依赖 entity/scene.h），定义在
// renderer.cpp —— 它需要完整的 MoleculeInstance 定义与场景实例顺序。
void buildInstanceRangeMask(const Representation& rep,
                            const std::vector<entity::MoleculeInstance>& instances,
                            std::vector<uint8_t>& out);

} // namespace moleculens::engine