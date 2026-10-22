// =============================================================================
// MolecuLens · entity · scene/scene.cpp —— 场景实现
// 规格来源：主方案 §4.4（registerStructure 去重 / addInstance 追加坐标段）
// 坐标语义：CoordsSoA::positions = 世界坐标 = 帧坐标 × 实例位姿（P4）
// =============================================================================

#include "entity/scene/scene.h"

#include <algorithm>

namespace moleculens::entity {

namespace {

// 实例的世界变换 = 体系位姿 × 实例位姿（整个体系作为一个整体对象渲染）。
// **唯一实现**：fillInstanceSegment（静态几何/重建）与 applyTrajectoryFrame（帧切
// 播放）都必须走它 —— 两处各写一份时曾经漂移过：帧切路径漏乘体系位姿，于是
// 用户平移整个体系后再播放，分子会被"拉回"到体系变换之外的位置。
glm::mat4 instanceWorldMatrix(const SceneStructure& scene, const MoleculeInstance& mi) {
    glm::mat4 m = mi.transform.matrix();
    if (mi.systemId != UINT32_MAX) {
        const uint32_t si = scene.systemIndexById(mi.systemId);
        if (si != UINT32_MAX) {
            m = scene.systems()[si].transform.matrix() * m;
        }
    }
    return m;
}

// 取实例帧坐标（帧 0 兜底），应用实例位姿（× 体系位姿）→ 世界坐标段
void fillInstanceSegment(SceneStructure& scene, const MoleculeInstance& mi,
                         std::vector<glm::vec3>& out) {
    const size_t n = scene.structures()[mi.structureIndex].elements.size();
    const glm::mat4 m = instanceWorldMatrix(scene, mi);

    const Timestep* frame = nullptr;
    if (mi.trajectory && !mi.trajectory->frames.empty()) {
        frame = &mi.trajectory->frames[0];
    }
    const size_t base = out.size();
    out.resize(base + n, glm::vec3(0.0f));
    for (size_t i = 0; i < n; ++i) {
        glm::vec3 local(0.0f);
        // 帧坐标对本实例是**局部**的：体系级轨迹时本实例只占其中一段，
        // 起点由 trajAtomOffset 给出（实例私有轨迹则为 0，行为不变）
        const size_t src = static_cast<size_t>(mi.trajAtomOffset) + i;
        if (frame && src < frame->positions.size()) {
            local = frame->positions[src];
        }
        out[base + i] = local;   // 先落原始帧坐标，稍后统一补齐 + 变换
    }
    // 补齐跨盒子边界的分子（与 applyTrajectoryFrame 同一处理与同一理由）
    if (frame != nullptr) {
        const auto& st = scene.structures()[mi.structureIndex];
        unwrapByComponents(out.data() + base, out.data() + base, n,
                           st.unwrapRoot, st.unwrapAdjOffset, st.unwrapAdj,
                           frame->box);
    }
    for (size_t i = 0; i < n; ++i) {
        out[base + i] = glm::vec3(m * glm::vec4(out[base + i], 1.0f));
    }
}

} // namespace

uint32_t SceneStructure::registerStructure(const MoleculeStructure& ms) {
    auto it = structureIndex_.find(ms.key);
    if (it != structureIndex_.end()) {
        return it->second;
    }
    const uint32_t index = static_cast<uint32_t>(structuresPool_.size());
    structuresPool_.push_back(ms);
    MoleculeStructure& stored = structuresPool_.back();

    // 预计算"按键连通的分子分量"：每个原子的根 = 分量内最小原子下标。
    // 无键原子自成一类；bonds 的 a/b 已在解析阶段保证 < 原子数。
    const size_t n = stored.elements.size();
    stored.unwrapRoot.resize(n);
    for (size_t i = 0; i < n; ++i) {
        stored.unwrapRoot[i] = static_cast<uint32_t>(i);
    }
    auto findRoot = [&stored](uint32_t x) {
        while (stored.unwrapRoot[x] != x) {
            stored.unwrapRoot[x] = stored.unwrapRoot[stored.unwrapRoot[x]];
            x = stored.unwrapRoot[x];
        }
        return x;
    };
    for (const Bond& b : stored.bonds) {
        if (b.a >= n || b.b >= n) {
            continue;
        }
        const uint32_t ra = findRoot(b.a);
        const uint32_t rb = findRoot(b.b);
        if (ra == rb) {
            continue;
        }
        // 根取较小下标，保证 unwrapRoot[i] == i 是分量锚点。
        if (ra < rb) {
            stored.unwrapRoot[rb] = ra;
        } else {
            stored.unwrapRoot[ra] = rb;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        stored.unwrapRoot[i] = findRoot(static_cast<uint32_t>(i));
    }

    // 键邻接 CSR（无向，两个方向都存）。逐帧 unwrap 沿键 BFS 传播镜像，
    // 保证每条键的两个端点处于相邻镜像，长链/环也不会被拆到盒子两侧。
    stored.unwrapAdjOffset.assign(n + 1, 0);
    for (const Bond& b : stored.bonds) {
        if (b.a < n && b.b < n) {
            ++stored.unwrapAdjOffset[b.a + 1];
            ++stored.unwrapAdjOffset[b.b + 1];
        }
    }
    for (size_t i = 1; i <= n; ++i) {
        stored.unwrapAdjOffset[i] += stored.unwrapAdjOffset[i - 1];
    }
    stored.unwrapAdj.assign(stored.unwrapAdjOffset[n], 0);
    std::vector<uint32_t> cursor = stored.unwrapAdjOffset;
    for (const Bond& b : stored.bonds) {
        if (b.a < n && b.b < n) {
            stored.unwrapAdj[cursor[b.a]++] = b.b;
            stored.unwrapAdj[cursor[b.b]++] = b.a;
        }
    }

    structureIndex_.emplace(stored.key, index);
    return index;
}

uint32_t SceneStructure::addInstance(uint32_t structureIndex, const Transform& tm,
                                     uint32_t systemId) {
    if (structureIndex >= structuresPool_.size()) {
        return UINT32_MAX;
    }
    MoleculeInstance mi;
    mi.structureIndex = structureIndex;
    mi.transform = tm;
    mi.name = "molecule_" + std::to_string(instances_.size());
    auto traj = std::make_shared<Trajectory>();
    traj->frames.emplace_back();  // 静态 1 帧
    mi.trajectory = traj;
    mi.systemId = resolveSystemId(systemId);

    const uint32_t index = static_cast<uint32_t>(instances_.size());
    instances_.push_back(std::move(mi));
    rebuildSystemMembership();

    // 坐标段追加：该结构长度 × 1 帧（世界坐标 = 帧坐标 × 位姿）
    fillInstanceSegment(*this, instances_[index], coordsSoA_.positions);
    const size_t n = structuresPool_[structureIndex].elements.size();
    coordsSoA_.colors.resize(coordsSoA_.colors.size() + n, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
    const uint32_t base = static_cast<uint32_t>(coordsSoA_.positions.size()) - static_cast<uint32_t>(n);
    for (size_t i = 0; i < n; ++i) {
        coordsSoA_.atomIds.push_back(base + static_cast<uint32_t>(i));
    }
    ++coordsSoA_.version;

    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
    return index;
}

uint32_t SceneStructure::addTrajectoryInstance(uint32_t structureIndex,
                                               const Transform& tm,
                                               std::shared_ptr<Trajectory> traj,
                                               uint32_t systemId,
                                               const std::string& name,
                                               bool isSolvent) {
    if (structureIndex >= structuresPool_.size() || !traj || traj->frames.empty()) {
        return UINT32_MAX;
    }
    MoleculeInstance mi;
    mi.structureIndex = structureIndex;
    mi.transform = tm;
    mi.name = name.empty() ? ("molecule_" + std::to_string(instances_.size())) : name;
    mi.trajectory = std::move(traj);
    mi.systemId = resolveSystemId(systemId);
    mi.isSolvent = isSolvent;

    const uint32_t index = static_cast<uint32_t>(instances_.size());
    instances_.push_back(std::move(mi));
    rebuildSystemMembership();

    fillInstanceSegment(*this, instances_[index], coordsSoA_.positions);
    const size_t n = structuresPool_[structureIndex].elements.size();
    coordsSoA_.colors.resize(coordsSoA_.colors.size() + n, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
    const uint32_t base = static_cast<uint32_t>(coordsSoA_.positions.size()) - static_cast<uint32_t>(n);
    for (size_t i = 0; i < n; ++i) {
        coordsSoA_.atomIds.push_back(base + static_cast<uint32_t>(i));
    }
    ++coordsSoA_.version;

    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
    return index;
}

uint32_t SceneStructure::addSystem(const std::string& name) {
    SystemGroup sys;
    sys.id = nextSystemId_++;
    sys.name = name.empty() ? ("system_" + std::to_string(systems_.size())) : name;
    systems_.push_back(std::move(sys));
    if (currentSystemId_ == UINT32_MAX) {
        currentSystemId_ = sys.id;   // 首个体系自动设为当前
    }
    setDirty(DIRTY_INSTANCE);
    return sys.id;
}

bool SceneStructure::removeSystem(uint32_t systemId) {
    const uint32_t si = systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    // 删除该体系全部实例（成员下标已失效，全部擦除后整体重建成员表）
    std::vector<uint32_t> victims = systems_[si].instanceIndices;
    std::sort(victims.begin(), victims.end(), std::greater<uint32_t>());
    for (const uint32_t idx : victims) {
        instances_.erase(instances_.begin() + idx);
    }
    rebuildSystemMembership();
    systems_.erase(systems_.begin() + si);
    if (currentSystemId_ == systemId) {
        currentSystemId_ = systems_.empty() ? UINT32_MAX : systems_.front().id;
    }
    rebuildCoordsSoA();
    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
    return true;
}

bool SceneStructure::setSystemVisible(uint32_t systemId, bool visible) {
    const uint32_t si = systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    systems_[si].visible = visible;
    setDirty(DIRTY_INSTANCE);
    return true;
}

bool SceneStructure::setSystemTransform(uint32_t systemId, const Transform& tm) {
    const uint32_t si = systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    systems_[si].transform = tm;
    rebuildCoordsSoA();   // 体系位姿变更 → 成员世界坐标重算
    // 纯位姿变更不动拓扑（原子数/键数/表示都不变）→ 只需 DIRTY_COORDS。
    // 置 DIRTY_INSTANCE 会强制整条命令流重建；改为 DIRTY_COORDS 后由 Renderer 的
    // 坐标重放路径处理，拖动体系时不再每帧重建（主方案 §5.8.2 坐标编辑粒度）。
    setDirty(DIRTY_COORDS);
    return true;
}

bool SceneStructure::setSystemRenderMode(uint32_t systemId, SystemRenderMode mode) {
    const uint32_t si = systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    systems_[si].renderMode = mode;
    setDirty(DIRTY_INSTANCE);   // 模式变更 → 命令流重建（RenderMode 命令重发）
    return true;
}

bool SceneStructure::renameSystem(uint32_t systemId, const std::string& name) {
    const uint32_t si = systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    systems_[si].name = name;
    setDirty(DIRTY_INSTANCE);
    return true;
}

bool SceneStructure::moveInstanceToSystem(uint32_t instanceIndex, uint32_t systemId) {
    if (instanceIndex >= instances_.size() || systemIndexById(systemId) == UINT32_MAX) {
        return false;
    }
    instances_[instanceIndex].systemId = systemId;
    rebuildSystemMembership();
    rebuildCoordsSoA();
    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
    return true;
}

uint32_t SceneStructure::systemIdOfInstance(uint32_t instanceIndex) const {
    if (instanceIndex >= instances_.size()) {
        return UINT32_MAX;
    }
    return instances_[instanceIndex].systemId;
}

uint32_t SceneStructure::systemIndexById(uint32_t systemId) const {
    for (size_t i = 0; i < systems_.size(); ++i) {
        if (systems_[i].id == systemId) {
            return static_cast<uint32_t>(i);
        }
    }
    return UINT32_MAX;
}

bool SceneStructure::setCurrentSystem(uint32_t systemId) {
    if (systemIndexById(systemId) == UINT32_MAX) {
        return false;
    }
    currentSystemId_ = systemId;
    return true;
}

uint64_t SceneStructure::bumpSerial() {
    ++serial_;
    return serial_;
}

void SceneStructure::removeInstance(uint32_t instanceIndex) {
    if (instanceIndex >= instances_.size()) {
        return;
    }
    instances_.erase(instances_.begin() + instanceIndex);
    rebuildSystemMembership();   // 实例下标前移 → 体系成员表整体重建
    rebuildCoordsSoA();
    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
}

void SceneStructure::setInstanceVisible(uint32_t instanceIndex, bool visible) {
    if (instanceIndex >= instances_.size() || instances_[instanceIndex].visible == visible) {
        return;
    }
    instances_[instanceIndex].visible = visible;
    setDirty(DIRTY_INSTANCE);
}

bool SceneStructure::setSolventVisible(bool visible) {
    bool changed = false;
    for (auto& inst : instances_) {
        if (inst.isSolvent && inst.visible != visible) {
            inst.visible = visible;
            changed = true;
        }
    }
    if (changed) {
        setDirty(DIRTY_INSTANCE);
    }
    return changed;
}

void SceneStructure::setInstanceTransform(uint32_t instanceIndex, const Transform& tm) {
    if (instanceIndex >= instances_.size()) {
        return;
    }
    instances_[instanceIndex].transform = tm;
    // 位姿变更必须重算世界坐标（此前只置脏，rebuildCoordsSoA 无调用点 →
    // 移动分子后渲染仍用旧坐标，屏幕不变——见 moleculens-fixes）
    rebuildCoordsSoA();
    // 纯位姿变更不动拓扑 → 只需 DIRTY_COORDS（见 setSystemTransform 注释）
    setDirty(DIRTY_COORDS);
}

void SceneStructure::writeFramePositions(uint32_t instanceIndex,
                                         const std::vector<glm::vec3>& worldPositions) {
    if (instanceIndex >= instances_.size()) {
        return;
    }
    const size_t n = structuresPool_[instances_[instanceIndex].structureIndex].elements.size();
    if (worldPositions.size() < n) {
        return;
    }
    const size_t base = globalAtomBase(instanceIndex);
    for (size_t i = 0; i < n; ++i) {
        coordsSoA_.positions[base + i] = worldPositions[i];
    }
    ++coordsSoA_.version;
    setDirty(DIRTY_COORDS);
}

bool SceneStructure::applyTrajectoryFrame(uint32_t instanceIndex, uint64_t frame) {
    if (instanceIndex >= instances_.size()) {
        return false;
    }
    const auto& inst = instances_[instanceIndex];
    if (!inst.trajectory || frame >= inst.trajectory->frames.size()) {
        return false;
    }
    // 作用域 = 本实例 + 所有共享同一 Trajectory 对象的实例（体系级轨迹即整个体系）。
    // 共享同一对象意味着"同一份文件的同一帧"，它们必须一起推进。
    const Trajectory* shared = inst.trajectory.get();
    const auto& positions = inst.trajectory->frames[frame].positions;

    bool primaryOk = false;
    bool wroteAny = false;
    // 暂存缓冲提到循环外：多实例体系（md.gro 是 2042 个实例）时每帧都要为每个
    // 实例做一次 vector 分配 —— 实测这是帧切最贵的一段。提到外面后 capacity 在
    // 实例之间复用（同类实例原子数相近），不再逐实例分配/释放。
    std::vector<glm::vec3> world;
    // 段起点随 instances_ 顺序**增量**推进：旧实现每实例调 writeFramePositions
    // → globalAtomBase(i) 内部 O(i) 累加 → 整体 O(n²)（2042 实例实测 23 ms/帧）。
    // 整段 O(n)，脏位/坐标版本号只置一次（语义与逐实例置脏等价）。
    uint32_t base = 0;
    for (uint32_t i = 0; i < instances_.size(); ++i) {
        const auto& mi = instances_[i];
        const auto& st = structuresPool_[mi.structureIndex];
        const size_t n = st.elements.size();
        const bool sharedTraj = mi.trajectory.get() == shared;
        // 尺寸校验：帧坐标长度不足以覆盖本实例的切片时拒绝写入。
        // （多帧 PDB 曾因"拓扑多采了其它 model 的原子、帧只有 1/3 长度"而静默
        //  拒绝写入 → 帧切永远不动；此处把"静默无反应"变成可判定的失败。）
        const size_t off = mi.trajAtomOffset;
        if (sharedTraj && off + n <= positions.size() &&
            static_cast<size_t>(base) + n <= coordsSoA_.positions.size()) {
            // 与静态几何路径共用同一变换合成（曾因此处漏乘体系位姿而漂移）
            const glm::mat4 m = instanceWorldMatrix(*this, mi);
            world.resize(n);
            for (size_t k = 0; k < n; ++k) {
                world[k] = positions[off + k];
            }
            // 补齐跨盒子边界的分子**再**做位姿变换：轨迹文件（XTC/DCD）的坐标同样是
            // wrapped 的，若不补齐，播放到该帧时分子又会被画成长棍（结构文件补齐了、
            // 逐帧坐标没补齐 = 只有首帧正常，播放后立刻变形）。
            unwrapByComponents(world.data(), world.data(), world.size(), st.unwrapRoot,
                               st.unwrapAdjOffset, st.unwrapAdj,
                               inst.trajectory->frames[frame].box);
            for (size_t k = 0; k < n; ++k) {
                coordsSoA_.positions[base + k] =
                    glm::vec3(m * glm::vec4(world[k], 1.0f));
            }
            wroteAny = true;
            if (i == instanceIndex) {
                primaryOk = true;
            }
        }
        base += static_cast<uint32_t>(n);
    }
    if (wroteAny) {
        ++coordsSoA_.version;
        setDirty(DIRTY_COORDS);
    }
    return primaryOk;
}

bool SceneStructure::buildWorldPositionsForFrame(uint64_t frame,
                                                 std::vector<glm::vec3>& out) const {
    out.clear();
    out.reserve(coordsSoA_.positions.size());
    for (const auto& mi : instances_) {
        const auto& st = structuresPool_[mi.structureIndex];
        const size_t n = st.elements.size();
        const size_t base = out.size();
        out.resize(base + n, glm::vec3(0.0f));

        const Timestep* ts = nullptr;
        if (mi.trajectory && !mi.trajectory->frames.empty()) {
            const uint64_t f = std::min<uint64_t>(frame, mi.trajectory->frames.size() - 1);
            ts = &mi.trajectory->frames[f];
        }
        if (ts != nullptr) {
            // 轨迹是 wrapped 的：先补齐再变换（与 applyTrajectoryFrame 完全一致）
            for (size_t k = 0; k < n; ++k) {
                const size_t src = static_cast<size_t>(mi.trajAtomOffset) + k;
                if (src < ts->positions.size()) {
                    out[base + k] = ts->positions[src];
                }
            }
            unwrapByComponents(out.data() + base, out.data() + base, n,
                               st.unwrapRoot, st.unwrapAdjOffset, st.unwrapAdj, ts->box);
        }
        const glm::mat4 m = instanceWorldMatrix(*this, mi);
        for (size_t k = 0; k < n; ++k) {
            out[base + k] = glm::vec3(m * glm::vec4(out[base + k], 1.0f));
        }
    }
    return !out.empty();
}

bool SceneStructure::setSystemTrajectory(uint32_t systemId,
                                        std::shared_ptr<Trajectory> traj) {
    if (!traj || traj->frames.empty()) {
        return false;
    }
    const uint32_t si = systemIndexById(systemId);
    if (si == UINT32_MAX) {
        return false;
    }
    // 成员按 instances_ 顺序（= 导入顺序 = 文件原子顺序）串接，逐实例分配偏移。
    // 必须与 CoordsSoA 的排布一致 —— fillInstanceSegment/globalAtomBase 都按
    // instances_ 顺序累加，这里用同一顺序才能保证"轨迹切片"与"坐标段"对齐。
    const uint32_t total =
        static_cast<uint32_t>(traj->frames[0].positions.size());
    uint32_t offset = 0;
    std::vector<uint32_t> members;
    for (uint32_t i = 0; i < instances_.size(); ++i) {
        if (instances_[i].systemId == systemId) {
            members.push_back(i);
        }
    }
    if (members.empty()) {
        return false;
    }
    for (uint32_t i : members) {
        offset += static_cast<uint32_t>(
            structuresPool_[instances_[i].structureIndex].elements.size());
    }
    if (offset != total) {
        // 不做部分挂载：长度不符意味着轨迹与本体系不同源，挂上去只会让每个分子
        // 都拿到错误的坐标段（比"导入失败"更难排查）
        return false;
    }

    offset = 0;
    for (uint32_t i : members) {
        instances_[i].trajectory = traj;   // 共享同一对象（见 MoleculeInstance 注释）
        instances_[i].trajAtomOffset = offset;
        offset += static_cast<uint32_t>(
            structuresPool_[instances_[i].structureIndex].elements.size());
    }
    // 用首帧刷新坐标段（与 setInstanceTrajectory 的分工一致：登记 + 置脏，正式
    // 发布首帧由时间轴 publishFrame 统一负责；此处刷新是为了导入后立即看到首帧）
    applyTrajectoryFrame(members.front(), 0);
    setDirty(DIRTY_COORDS | DIRTY_INSTANCE);
    ++serial_;
    return true;
}

void SceneStructure::rebuildCoordsSoA() {
    coordsSoA_.positions.clear();
    coordsSoA_.colors.clear();
    coordsSoA_.atomIds.clear();
    for (const MoleculeInstance& mi : instances_) {
        fillInstanceSegment(*this, mi, coordsSoA_.positions);
        const size_t n = structuresPool_[mi.structureIndex].elements.size();
        coordsSoA_.colors.resize(coordsSoA_.colors.size() + n, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
        const uint32_t base = static_cast<uint32_t>(coordsSoA_.positions.size()) -
                              static_cast<uint32_t>(n);
        for (size_t i = 0; i < n; ++i) {
            coordsSoA_.atomIds.push_back(base + static_cast<uint32_t>(i));
        }
    }
    ++coordsSoA_.version;
}

uint32_t SceneStructure::globalAtomBase(uint32_t instanceIndex) const {
    uint32_t base = 0;
    for (uint32_t i = 0; i < instanceIndex && i < instances_.size(); ++i) {
        base += static_cast<uint32_t>(
            structuresPool_[instances_[i].structureIndex].elements.size());
    }
    return base;
}

bool SceneStructure::setInstanceTrajectory(uint32_t instanceIndex,
                                          std::shared_ptr<Trajectory> traj) {
    if (instanceIndex >= instances_.size()) {
        return false;
    }
    // 注意：只登记轨迹，**不在此处写坐标段** —— 帧坐标是结构局部坐标，需要
    // 乘实例位姿才是世界坐标；该变换由时间轴 publishFrame 统一负责（避免同一
    // 语义在两处各自实现）。调用方随后广播 SceneOpened，由时间轴激活并发布首帧。
    instances_[instanceIndex].trajectory = std::move(traj);
    setDirty(DIRTY_COORDS | DIRTY_INSTANCE);
    ++serial_;
    return true;
}

void SceneStructure::clear() {
    structuresPool_.clear();
    structureIndex_.clear();
    instances_.clear();
    systems_.clear();
    chainNames_.clear();
    residueNames_.clear();
    coordsSoA_ = CoordsSoA{};
    props_ = SceneProperties{};
    strings_ = StringPool{};
    nextSystemId_ = 1;
    currentSystemId_ = UINT32_MAX;
    dirty_ = DIRTY_ALL;   // 清空本身即变更
    ++serial_;
}

int32_t SceneStructure::addChain(std::string_view name) {
    chainNames_.push_back(strings_.intern(name));
    return static_cast<int32_t>(chainNames_.size()) - 1;
}

int32_t SceneStructure::addResidue(std::string_view name) {
    residueNames_.push_back(strings_.intern(name));
    return static_cast<int32_t>(residueNames_.size()) - 1;
}

std::string_view SceneStructure::chainName(int32_t chainId) const {
    if (chainId < 0 || static_cast<size_t>(chainId) >= chainNames_.size()) {
        return {};
    }
    return strings_.resolve(chainNames_[static_cast<size_t>(chainId)]);
}

std::string_view SceneStructure::residueName(int32_t residueId) const {
    if (residueId < 0 || static_cast<size_t>(residueId) >= residueNames_.size()) {
        return {};
    }
    return strings_.resolve(residueNames_[static_cast<size_t>(residueId)]);
}

std::string_view SceneStructure::atomName(uint32_t structureIndex, uint32_t atomIndex) const {
    if (structureIndex >= structuresPool_.size()) {
        return {};
    }
    const MoleculeStructure& ms = structuresPool_[structureIndex];
    if (atomIndex >= ms.atomNameIds.size()) {
        return {};
    }
    return strings_.resolve(ms.atomNameIds[atomIndex]);
}

uint32_t SceneStructure::resolveSystemId(uint32_t requested) {
    if (requested != UINT32_MAX && systemIndexById(requested) != UINT32_MAX) {
        return requested;
    }
    if (currentSystemId_ != UINT32_MAX && systemIndexById(currentSystemId_) != UINT32_MAX) {
        return currentSystemId_;
    }
    return addSystem();   // 无可用体系 → 自动新建
}

void SceneStructure::rebuildSystemMembership() {
    for (auto& sys : systems_) {
        sys.instanceIndices.clear();
    }
    for (uint32_t i = 0; i < instances_.size(); ++i) {
        const uint32_t si = systemIndexById(instances_[i].systemId);
        if (si != UINT32_MAX) {
            systems_[si].instanceIndices.push_back(i);
        }
    }
}

} // namespace moleculens::entity
