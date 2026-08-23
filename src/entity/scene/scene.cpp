// =============================================================================
// MolecuLens · entity · scene/scene.cpp —— 场景实现
// 规格来源：主方案 §4.4（registerStructure 去重 / addInstance 追加坐标段）
// 坐标语义：CoordsSoA::positions = 世界坐标 = 帧坐标 × 实例位姿（P4）
// =============================================================================

#include "entity/scene/scene.h"

#include <algorithm>

namespace moleculens::entity {

namespace {

// 取实例帧坐标（帧 0 兜底），应用实例位姿（× 体系位姿）→ 世界坐标段
void fillInstanceSegment(SceneStructure& scene, const MoleculeInstance& mi,
                         std::vector<glm::vec3>& out) {
    const size_t n = scene.structures()[mi.structureIndex].elements.size();
    // 体系位姿合成：整个体系作为一个整体对象渲染（体系变换 × 实例变换）
    glm::mat4 m = mi.transform.matrix();
    if (mi.systemId != UINT32_MAX) {
        const uint32_t si = scene.systemIndexById(mi.systemId);
        if (si != UINT32_MAX) {
            m = scene.systems()[si].transform.matrix() * m;
        }
    }

    const std::vector<glm::vec3>* frame = nullptr;
    if (mi.trajectory && !mi.trajectory->frames.empty()) {
        frame = &mi.trajectory->frames[0].positions;
    }
    const size_t base = out.size();
    out.resize(base + n, glm::vec3(0.0f));
    for (size_t i = 0; i < n; ++i) {
        glm::vec3 local(0.0f);
        if (frame && i < frame->size()) {
            local = (*frame)[i];
        }
        out[base + i] = glm::vec3(m * glm::vec4(local, 1.0f));
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
    structureIndex_.emplace(ms.key, index);
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
                                               uint32_t systemId) {
    if (structureIndex >= structuresPool_.size() || !traj || traj->frames.empty()) {
        return UINT32_MAX;
    }
    MoleculeInstance mi;
    mi.structureIndex = structureIndex;
    mi.transform = tm;
    mi.name = "molecule_" + std::to_string(instances_.size());
    mi.trajectory = std::move(traj);
    mi.systemId = resolveSystemId(systemId);

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
    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
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

void SceneStructure::setInstanceTransform(uint32_t instanceIndex, const Transform& tm) {
    if (instanceIndex >= instances_.size()) {
        return;
    }
    instances_[instanceIndex].transform = tm;
    // 位姿变更必须重算世界坐标（此前只置脏，rebuildCoordsSoA 无调用点 →
    // 移动分子后渲染仍用旧坐标，屏幕不变——见 moleculens-fixes）
    rebuildCoordsSoA();
    setDirty(DIRTY_INSTANCE | DIRTY_COORDS);
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
