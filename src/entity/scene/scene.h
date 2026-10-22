// =============================================================================
// MolecuLens · entity · scene/scene.h —— 场景（SceneStructure / CoordsSoA / 脏位域）
// 规格来源：主方案 §4.3 / §4.4（AoS/SoA 混合；去重注册；脏位域 + serial）
// =============================================================================

#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cmath>   // std::round（minimumImageDelta）
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
// 脏位语义（与 Renderer::update 的重建粒度一一对应，主方案 §5.4/§5.8.2）：
//   DIRTY_COORDS         命令序列不变、只有坐标变 → 只重发坐标类命令段（坐标重放）
//   DIRTY_STRUCTURE/     命令序列本身会变（原子/键/实例/表示） → 整条命令流重建
//   DIRTY_INSTANCE/
//   DIRTY_REPRESENTATION
//   DIRTY_PROPERTIES     只进 GlobalUBO（背景/光照/雾/缩放）→ 命令流无需重建
// 注意：实例/体系的**位姿**变更只改坐标、不改拓扑，故归 DIRTY_COORDS；
//       实例的增删与显隐才改拓扑，归 DIRTY_INSTANCE。
enum DirtyBits : uint32_t {
    DIRTY_COORDS         = 1u << 0,      // 帧切 / 坐标编辑 / 位姿变更
    DIRTY_STRUCTURE      = 1u << 1,      // 拓扑增删
    DIRTY_INSTANCE       = 1u << 2,      // 实例增删 / 显隐（拓扑变更）
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
    float ambient = 0.45f;                   // 半球光强度（0 纯平 / 越大天顶地面差异越大）
    float scale = 1.0f;                      // 全局缩放
    // —— 剖切面（v2.3，运行期；本轮不写入 .mlscene）——
    // 世界空间平面 dot(p, n) + d = 0；保留 <=0 一侧，另一侧在片元着色器 discard。
    // 与 ChimeraX 的 clip 语义一致：法线指向"被切掉"的一侧。
    bool clipEnabled = false;
    float clipPlane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    Units units = Units::Angstrom;
    // —— 全局光（环境光/全局光照，无方向光/点光源）——
    // 全局 UBO 语义：ambient=基础环境色+强度；skyColor/groundColor 构成半球环境光
    // （天顶亮/地面暗的渐变，提供深度感而不需要方向光）。
    float skyColor[3] = {1.0f, 1.0f, 1.0f};       // 天顶（半球上方）色
    float groundColor[3] = {0.35f, 0.4f, 0.5f};   // 地面（半球下方）色
    float ambientColor[3] = {0.55f, 0.6f, 0.75f}; // 均匀环境光基础色
    // —— 方向光（主方案 §6.6.4；与 LightParams 默认值保持一致）——
    float dirLightDir[3] = {0.40f, 0.80f, 0.60f}; // 方向（指向光源）
    float dirLightColor[3] = {1.0f, 0.98f, 0.95f};
    float dirLightIntensity = 0.35f;              // 0 = 关
    bool  dirLightEnabled = true;
    // 方向光阴影（深度 pass + PCF）
    bool  shadowsEnabled = true;
    float shadowStrength = 0.65f;   // 0 = 无阴影，1 = 全黑本影
};

// —— 单帧（SoA 扁平；主方案 §4.3）——
struct Timestep {
    std::vector<glm::vec3> positions;    // Å（加载时统一换算）
    std::vector<glm::vec3> velocities;   // 可选
    std::array<float, 9>   box{};        // 周期盒子（可选）
};

// =============================================================================
// 周期边界（PBC）坐标处理 —— 显示前必须"补齐分子"
// =============================================================================
// 问题：结构/轨迹文件里的坐标常是**跨盒子边界被拆开**的（wrapped）。一个分子的
//   一部分在盒子这端、另一部分在另一端：两者在化学上相邻（最小镜像距离 ~1 Å），
//   但**原始坐标差**接近一个盒长。判键（BondSearch）用的是最小镜像，判定是对的；
//   而渲染按原始坐标连键 → 画出一根 ~38 Å 的长棍。实测 data/md.gro 有 20 根
//   （盒长 40.28 Å），画面上是几根莫名其妙的细棍，很难归因。
// 做法：以某个锚原子为基准，把其余原子按最小镜像位移拉回锚所在的那个镜像。
//   分子被"补齐"成连续几何 → 键自然变回正常长度。
// 范围：仅正交盒子（取 box[0]/box[4]/box[8] 三个对角元）。GRO/XTC 的常见输出即
//   正交盒；三斜盒需完整矩阵求逆，当前不做（盒子非正交时本函数退化为恒等变换，
//   即维持原行为，不会引入错误）。
inline glm::vec3 minimumImageDelta(const glm::vec3& d, const float boxLen[3]) {
    glm::vec3 r = d;
    for (int i = 0; i < 3; ++i) {
        if (boxLen[i] > 1e-3f) {
            r[i] -= boxLen[i] * std::round(d[i] / boxLen[i]);
        }
    }
    return r;
}

// 以 pts[0] 为锚补齐整段坐标（原地）。count == 0 时无操作。
inline void unwrapAroundFirst(glm::vec3* pts, size_t count,
                             const std::array<float, 9>& box) {
    if (pts == nullptr || count == 0) {
        return;
    }
    const float L[3] = {box[0], box[4], box[8]};
    if (L[0] <= 1e-3f && L[1] <= 1e-3f && L[2] <= 1e-3f) {
        return;   // 非正交/无盒子 → 不动（保持原行为）
    }
    const glm::vec3 anchor = pts[0];
    for (size_t i = 1; i < count; ++i) {
        pts[i] = anchor + minimumImageDelta(pts[i] - anchor, L);
    }
}

// —— 轨迹（主方案 §4.3 / §8.1）——
struct Trajectory {
    std::vector<Timestep> frames;        // 各帧坐标（共享拓扑）
    float dt = 2.0f;                     // 帧间隔（ps）
    bool  periodic = false;              // 周期盒子
    std::string source;                  // 来源文件名（导入时写入；轨迹列表显示用）
};

// —— 分子实例（主方案 §4.3）——
struct MoleculeInstance {
    uint32_t structureIndex = 0;         // → structuresPool_
    Transform transform;                 // 平移 + 旋转 + 缩放
    std::string name;                    // 显示名
    bool visible = true;
    std::shared_ptr<Trajectory> trajectory;  // 静态文件则 frames.size()==1
    // 本实例第 0 个原子在 trajectory 帧坐标里的下标。
    // 0 = 该轨迹是"实例私有"的（每帧长度 == 本实例原子数，历史行为，不变）。
    // >0 或与同体系其他实例**共享同一个 Trajectory 对象**时，帧坐标是**体系级**
    // 的（长度 == 体系总原子数），本实例取 [trajAtomOffset, +原子数) 这一段。
    // 之所以让多实例共享同一个 Trajectory 对象（而非各切一份），是为了让
    // "同一体系必须同步播放"成为**结构上的必然**：见 applyTrajectoryFrame。
    uint32_t trajAtomOffset = 0;
    std::vector<uint32_t> globalAtomIds;     // 实例内原子 → 全局原子 ID（拾取回查）
    uint32_t systemId = UINT32_MAX;      // 所属体系（SystemGroup.id）；无 = 未归组
    bool isSolvent = false;              // 溶剂分子（水等，可整体显隐）；默认否
};

// —— 体系 / 组（一次导入的多分子文件视为一个体系；主方案 §4.3 扩展）——
// 体系是整体渲染与管理单位：体系级位姿合成到成员实例（世界坐标 = 体系位姿 ×
// 实例位姿），体系显隐影响整个体系；支持分子跨体系移交/重组。
// 每个体系可独立设置渲染模式（实心/线框/透明/点），互不影响 —— 管线按模式
// 创建/切换（SphereInst/CylinderInst 等按体系模式走对应 VkPipeline）。
enum class SystemRenderMode : uint8_t {
    Solid = 0,       // 实心（默认）：填充三角网格
    Wireframe,       // 线框：多边形 LINE 模式（三角网格线框）
    Transparent,     // 透明：混合（alpha）+ 降低材质透明度
    Point,           // 点：原子渲染为点、键渲染为线段
};
struct SystemGroup {
    uint32_t id = 0;                     // 稳定 id（实例按 id 关联，非下标）
    std::string name;                    // 显示名
    bool visible = true;
    Transform transform;                 // 体系级位姿（整体平移/旋转/缩放）
    std::vector<uint32_t> instanceIndices;  // 成员实例下标（随实例增删重建）
    SystemRenderMode renderMode = SystemRenderMode::Solid;   // 体系独立渲染模式
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
    // systemId = UINT32_MAX 时挂到当前体系（无则自动新建）

    uint32_t addInstance(uint32_t structureIndex, const Transform&,
                         uint32_t systemId = UINT32_MAX);

    // 追加轨迹实例（坐标段按帧数预留首帧）
    // name：实例显示名（空 = 默认 molecule_N）；isSolvent：溶剂标记（可整体显隐）
    uint32_t addTrajectoryInstance(uint32_t structureIndex, const Transform&,
                                   std::shared_ptr<Trajectory> traj,
                                   uint32_t systemId = UINT32_MAX,
                                   const std::string& name = std::string(),
                                   bool isSolvent = false);


    // —— 体系（SystemGroup）管理 ——
    uint32_t addSystem(const std::string& name = std::string());
    bool removeSystem(uint32_t systemId);                 // 删除体系及其全部实例
    bool setSystemVisible(uint32_t systemId, bool visible);
    bool setSystemTransform(uint32_t systemId, const Transform&);
    bool renameSystem(uint32_t systemId, const std::string& name);
    bool moveInstanceToSystem(uint32_t instanceIndex, uint32_t systemId);  // 跨组移交
    bool setSystemRenderMode(uint32_t systemId, SystemRenderMode mode);   // 体系渲染模式
    [[nodiscard]] const std::vector<SystemGroup>& systems() const { return systems_; }

    // 实例 → 所属体系 id（UINT32_MAX = 未归组）
    [[nodiscard]] uint32_t systemIdOfInstance(uint32_t instanceIndex) const;
    [[nodiscard]] uint32_t systemIndexById(uint32_t systemId) const;    // id → systems_ 下标
    [[nodiscard]] uint32_t currentSystemId() const { return currentSystemId_; }
    bool setCurrentSystem(uint32_t systemId);

    // 清空为初始状态（载入 .mlscene 等"整体替换场景"的场景需要；此前的唯一
    // 入口是 mergeParsedScene 式的追加合并，无法替换）。置 DIRTY_ALL 并递增 serial。
    void clear();

    void setDirty(uint32_t bits) { dirty_ |= bits; }   // 接受 DIRTY_* 位或组合
    [[nodiscard]] uint32_t dirtyBits() const { return dirty_; }
    void clearDirty() { dirty_ = 0; }
    uint64_t bumpSerial();               // 全局 serial（P5）
    [[nodiscard]] uint64_t serial() const { return serial_; }

    void removeInstance(uint32_t instanceIndex);
    void setInstanceVisible(uint32_t instanceIndex, bool visible);
    // 设置实例轨迹（导入轨迹文件后调用）。帧坐标常驻内存；置 DIRTY_COORDS 并
    // 用首帧刷新坐标段。传 nullptr 清除轨迹（退回静态单帧）。
    bool setInstanceTrajectory(uint32_t instanceIndex, std::shared_ptr<Trajectory> traj);
    // 体系级轨迹：把一条**整体系**的轨迹挂到该体系全部实例上（同一 Trajectory
    // 对象共享，各实例按几何顺序得到自己的 trajAtomOffset）。
    // 为什么需要它：GRO 解析器按残基把体系拆成多个分子实例（实测 data/md.gro →
    // 2042 实例 / 6452 原子），而 DCD/XTC 的每帧包含**整个体系**的坐标 ——
    // 用 setInstanceTrajectory 只能把 6452 个坐标挂到某一个 30 原子的分子上。
    // 校验：体系总原子数必须等于轨迹每帧坐标数，否则拒绝（不做部分挂载）。
    bool setSystemTrajectory(uint32_t systemId, std::shared_ptr<Trajectory> traj);
    void setInstanceTransform(uint32_t instanceIndex, const Transform&);
    // 溶剂整体显隐：按实例 isSolvent 标记置 visible（溶剂分子可在分子列表折叠，
    // 见 io/gro 溶剂读取；置 DIRTY_INSTANCE 触发重渲染）
    bool setSolventVisible(bool visible);

    // 写入当前帧坐标 → CoordsSoA（帧切路径，P4）
    void writeFramePositions(uint32_t instanceIndex, const std::vector<glm::vec3>& worldPositions);

    // 把第 frame 帧写进坐标段：帧坐标是结构局部坐标，需乘实例位姿才是世界坐标。
    // **唯一实现**——时间轴播放（TimelineController::publishFrame）与动画导出
    // （逐帧导出 PNG 序列）都调这里，避免同一变换在两处各自实现。
    //
    // 作用范围是"该实例 + 所有与它**共享同一 Trajectory 对象**的实例"（体系级
    // 轨迹时即整个体系）。这使"同体系必须同步播放"成为结构上的必然：若只推进
    // 当前实例，多实例体系会出现"只有一个分子在动、其余静止"的静默错误，而
    // 逐实例挂载的模型无法从数据上区分这种情况。
    // 返回 false：无轨迹 / 实例越界 / 帧越界 / 帧坐标长度与本实例原子数不符
    //（坐标段保持不变）。
    bool applyTrajectoryFrame(uint32_t instanceIndex, uint64_t frame);
    // 只读预测：按实例顺序生成第 frame 帧的**世界坐标**（不修改 coordsSoA、不置脏）。
    // QuickSurf 逐帧预计算用：播放时提前算好后续帧的表面，而不必把场景切到那一帧。
    // 无轨迹的实例按第 0 帧处理；短轨迹按末帧钳制。返回 false 仅当场景为空。
    bool buildWorldPositionsForFrame(uint64_t frame, std::vector<glm::vec3>& out) const;
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
    int32_t residueCount() const { return static_cast<int32_t>(residueNames_.size()); }

    // 原子名（结构 + 原子下标 → 名字符串）
    std::string_view atomName(uint32_t structureIndex, uint32_t atomIndex) const;

private:
    uint32_t resolveSystemId(uint32_t requested);          // UINT32_MAX → 当前/新建
    void rebuildSystemMembership();                        // 按 instances_ 重建成员表

    std::vector<MoleculeStructure> structuresPool_;

    std::unordered_map<StructureKey, uint32_t, StructureKeyHash> structureIndex_;
    std::vector<MoleculeInstance>  instances_;
    CoordsSoA coordsSoA_;
    SceneProperties props_;
    StringPool strings_;
    std::vector<uint32_t> chainNames_;     // StringPool id
    std::vector<uint32_t> residueNames_;   // StringPool id
    std::vector<SystemGroup> systems_;
    uint32_t nextSystemId_ = 1;
    uint32_t currentSystemId_ = UINT32_MAX;
    uint32_t dirty_ = 0;
    uint64_t serial_ = 0;
};

} // namespace moleculens::entity
