// =============================================================================
// MolecuLens · entity · scene/scene.h —— 场景（SceneStructure / CoordsSoA / 脏位域）
// 规格来源：主方案 §4.3 / §4.4（AoS/SoA 混合；去重注册；脏位域 + serial）
// =============================================================================

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "entity/atom/atom.h"
#include "entity/identity/identity.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/transform.h"
#include "entity/string_pool.h"

namespace moleculens::entity {

// —— 单位（主方案 §4.4：PDB:Å / GRO:nm 加载统一换算）——
enum class Units : uint8_t { Angstrom, Nanometer };

// —— 脏位域（主方案 §4.4）——
enum DirtyBits : uint32_t {
    DIRTY_COORDS         = 1u << 0,      // 帧切 / 坐标编辑
    DIRTY_STRUCTURE      = 1u << 1,      // 拓扑增删
    DIRTY_INSTANCE       = 1u << 2,      // 实例增删 / 位姿
    DIRTY_PROPERTIES     = 1u << 3,      // 背景/光照/单位
    DIRTY_REPRESENTATION = 1u << 4,      // 表示 / 配色变更
    DIRTY_ALL            = 0xFFFFFFFFu,
};

// —— 场景属性（主方案 §4.4）——
struct SceneProperties {
    float bgColor[3] = {0.1f, 0.1f, 0.15f};  // 背景色
    float fog[2] = {0.0f, 0.0f};             // 雾近/远（可选）
    float fogMode = 0.0f;                    // 0 关 / 1 线性 / 2 指数 / 3 指数²
    float fogDensity = 0.05f;
    float ambient = 0.3f;                    // 环境光强度
    float scale = 1.0f;                      // 全局缩放
    Units units = Units::Angstrom;
};

// —— 单帧（SoA 扁平；主方案 §4.3）——
struct Timestep {
    std::vector<glm::vec3> positions;    // Å（加载时统一换算）
    std::vector<glm::vec3> velocities;   // 可选
    std::array<float, 9>   box{};        // 周期盒子（可选）
};

// —— 轨迹（主方案 §4.3 / §8.1）——
struct Trajectory {
    std::vector<Timestep> frames;        // 各帧坐标（共享拓扑）
    float dt = 2.0f;                     // 帧间隔（ps）
    bool  periodic = false;              // 周期盒子
};

// —— 分子实例（主方案 §4.3）——
struct MoleculeInstance {
    uint32_t structureIndex = 0;         // → structuresPool_
    Transform transform;                 // 平移 + 旋转 + 缩放
    std::string name;                    // 显示名
    bool visible = true;
    std::shared_ptr<Trajectory> trajectory;  // 静态文件则 frames.size()==1
    std::vector<uint32_t> globalAtomIds;     // 实例内原子 → 全局原子 ID（拾取回查）
};

// —— 坐标 SoA（GPU 上传用，段式布局；主方案 §4.3）——
struct CoordsSoA {
    std::vector<glm::vec3> positions;    // 世界坐标，段长 = 结构原子数
    std::vector<glm::vec4> colors;       // 预计算色（含 alpha）
    std::vector<uint32_t>  atomIds;      // 拾取回查
    uint64_t version = 0;                // 坐标段版本（帧切只改这里）
};

// —— 帧数据（表示生成输入；主方案 §5.2）——
struct FrameData {
    const std::vector<glm::vec3>* positions = nullptr;  // 世界坐标（Å）
    const Timestep* timestep = nullptr;                 // 可选原始帧（速度等）
};

// —— 场景（主方案 §4.4）——
class SceneStructure {
public:
    // 去重注册：命中既有 → 返回既有下标；未命中 → 追加
    uint32_t registerStructure(const MoleculeStructure&);
    // 写入 instances_，并在 coordsSoA_ 追加该结构长度 × 1 帧的坐标段
    uint32_t addInstance(uint32_t structureIndex, const Transform&);
    // 追加轨迹实例（坐标段按帧数预留首帧）
    uint32_t addTrajectoryInstance(uint32_t structureIndex, const Transform&,
                                   std::shared_ptr<Trajectory> traj);

    void setDirty(uint32_t bits) { dirty_ |= bits; }   // 接受 DIRTY_* 位或组合
    uint32_t dirtyBits() const { return dirty_; }
    void clearDirty() { dirty_ = 0; }
    uint64_t bumpSerial();               // 全局 serial（P5）
    uint64_t serial() const { return serial_; }

    void removeInstance(uint32_t instanceIndex);
    void setInstanceVisible(uint32_t instanceIndex, bool visible);
    void setInstanceTransform(uint32_t instanceIndex, const Transform&);

    // 写入当前帧坐标 → CoordsSoA（帧切路径，P4）
    void writeFramePositions(uint32_t instanceIndex, const std::vector<glm::vec3>& worldPositions);
    // 重建 CoordsSoA（实例位姿/结构变更后）
    void rebuildCoordsSoA();

    // 静态几何（实例 + 结构）
    const std::vector<MoleculeStructure>& structures() const { return structuresPool_; }
    const std::vector<MoleculeInstance>&  instances() const { return instances_; }
    const CoordsSoA& coordsSoA() const { return coordsSoA_; }
    CoordsSoA& coordsSoA() { return coordsSoA_; }
    SceneProperties& properties() { return props_; }
    const SceneProperties& properties() const { return props_; }
    StringPool& strings() { return strings_; }
    const StringPool& strings() const { return strings_; }

    // 全局原子 ID 范围（拾取回查用）
    uint32_t globalAtomBase(uint32_t instanceIndex) const;

    // —— 链 / 残基名称表（PDB/GRO 解析时构建；选择语言用）——
    int32_t addChain(std::string_view name);
    int32_t addResidue(std::string_view name);
    std::string_view chainName(int32_t chainId) const;
    std::string_view residueName(int32_t residueId) const;
    int32_t chainCount() const { return static_cast<int32_t>(chainNames_.size()); }

    // 原子名（结构 + 原子下标 → 名字符串）
    std::string_view atomName(uint32_t structureIndex, uint32_t atomIndex) const;

private:
    std::vector<MoleculeStructure> structuresPool_;
    std::unordered_map<StructureKey, uint32_t, StructureKeyHash> structureIndex_;
    std::vector<MoleculeInstance>  instances_;
    CoordsSoA coordsSoA_;
    SceneProperties props_;
    StringPool strings_;
    std::vector<uint32_t> chainNames_;     // StringPool id
    std::vector<uint32_t> residueNames_;   // StringPool id
    uint32_t dirty_ = 0;
    uint64_t serial_ = 0;
};

} // namespace moleculens::entity
