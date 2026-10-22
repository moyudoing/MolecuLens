// =============================================================================
// MolecuLens · engine · render/renderer.cpp —— Renderer 实现
// 规格来源：主方案 §5.4 / §5.8.2（脏位域 → 收集任务 → 并行生成 → 保序合并 →
//           serial++；帧切只重建坐标段由 VulkanRenderer 处理）
// =============================================================================

#include "engine/render/renderer.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <glm/gtc/matrix_transform.hpp>   // lookAt / ortho（光源正交矩阵）

#include "spdlog/spdlog.h"

#include "engine/async/surface_jobs.h"
#include "engine/rep/representer.h"
#include "engine/raytrace/ray_tracer.h"          // RtScene/RtCamera/RtLight（GPU RT 入口）
#include "engine/style/espsurface/esp_surface.h"   // espOverlayReset（视口色标/极值标注）
#include "engine/style/quicksurf/quicksurf.h"
#include "engine/style/style_registry.h"   // styleFor（坐标重放直接调样式）
#include "engine/style/surface/surface.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "engine/vulkan/raytracing/gpu_ray_tracer.h"
#include "entity/element/element.h"
#include "entity/scene/scene.h"
#include "entity/selection/selection.h"

namespace moleculens::engine {

namespace {
// 网格拓扑是否随坐标恒定（可参与坐标重放）。Diamond/Plate/卡通类网格的
// 顶点数只由原子/残基拓扑决定；Surface/QuickSurf/ESP 的 MC 拓扑随每帧密度变化，
// 不能重放（尺寸不符会回退全量重建，但会白做一次）。
bool meshReplayAllowedFor(RepresentationType type) {
    switch (type) {
        case RepresentationType::Plate:
        case RepresentationType::Diamond:
        case RepresentationType::Cartoon:
        case RepresentationType::NewCartoon:
        case RepresentationType::Ribbon:
        case RepresentationType::Tube:
            return true;
        default:
            return false;
    }
}

// 当前可见表示是否全部允许网格重放。
// - 运动冻结时，**体系级**表面（整体系 QuickSurf / vdW）的 TriMesh 保留在旧命令池里，
//   重放只更新坐标命令 → 可以安全跳过表面段（见 rebuildCommandList 的 replay 分支）。
// - 非体系表面（每分子细分表面）没有冻结缓存，仍必须全量重建；否则重放会撞上
//   计划外的 TriMesh 命令（Debug 断言 / Release 静默丢面）。
// 冻结播放时是否还有"需要每帧更新坐标"的可见表示。
// 典型场景：只开一个整体系 QuickSurf 表面时，坐标帧切不改变任何命令（表面已冻结），
// 此时连重放都不必做：保留原命令流与 serial，后端就不会每帧重传 10 万+ 三角面。
bool sceneHasVisibleQuickSurf(const std::vector<Representation>& reps) {
    for (const auto& rep : reps) {
        if (rep.visible && rep.systemSurface &&
            rep.type == RepresentationType::QuickSurf) {
            return true;
        }
    }
    return false;
}

bool sceneNeedsCoordUpdate(const std::vector<Representation>& reps, bool freezeSurfaces,
                           bool freezeQuickSurf) {
    if (!freezeSurfaces && !freezeQuickSurf) {
        return true;
    }
    for (const auto& rep : reps) {
        if (!rep.visible) {
            continue;
        }
        const bool systemSurface =
            rep.systemSurface &&
            (rep.type == RepresentationType::Surface ||
             rep.type == RepresentationType::QuickSurf);
        if (!systemSurface) {
            return true;
        }
        const bool frozen = rep.type == RepresentationType::QuickSurf
                                ? freezeQuickSurf
                                : freezeSurfaces;
        if (!frozen) {
            return true;
        }
    }
    return false;
}

bool sceneMeshReplaySafe(const std::vector<Representation>& reps, bool freezeSurfaces,
                         bool freezeQuickSurf) {
    for (const auto& rep : reps) {
        if (!rep.visible) {
            continue;
        }
        const bool systemSurface =
            rep.systemSurface &&
            (rep.type == RepresentationType::Surface ||
             rep.type == RepresentationType::QuickSurf);
        if (systemSurface) {
            const bool frozen = rep.type == RepresentationType::QuickSurf
                                    ? freezeQuickSurf
                                    : freezeSurfaces;
            if (frozen) {
                continue;   // 旧表面网格原样保留
            }
            return false;   // 表面需要跟随新坐标重建
        }
        if (rep.type == RepresentationType::Surface ||
            rep.type == RepresentationType::QuickSurf ||
            rep.type == RepresentationType::EspSurface) {
            return false;
        }
        if (!meshReplayAllowedFor(rep.type)) {
            // 球/棍/线/点云不含 TriMesh，天然可重放；只有未知网格类才拒绝。
            continue;
        }
    }
    return true;
}

// 实例的"分子类型"token（v2.19 类型范围）：实例名形如 "SOL #12" / "molecule_3"。
// 取 '#' 之前的部分并 trim —— 同一类型的分子只有序号不同，范围过滤必须按类型分组，
// 否则"50% 的 SOL"无从谈起。无 '#' 时整个名字就是类型（"molecule_3" 的 3 是下标，
// 但名字本身仍是稳定的分组键，调用方/UI 给出的就是这串 token）。
std::string instanceTypeToken(const std::string& name) {
    const size_t hash = name.find('#');
    const std::string head = name.substr(0, hash);
    size_t b = 0;
    size_t e = head.size();
    while (b < e && std::isspace(static_cast<unsigned char>(head[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(head[e - 1])) != 0) {
        --e;
    }
    return head.substr(b, e - b);
}

// 规格里的 rangeType 可能带用户输入的空白，比较前同样 trim。
std::string trimToken(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}
} // namespace

// 实例级类型范围掩码（v2.19）。语义见 representation.h 的 rangeMode 注释：
//   · 先按 rangeType 过滤（空 = 全部类型）；
//   · 匹配实例**按场景顺序**编号 p（0 起），Fraction 取前 ceil(fraction*N) 个，
//     Stride 取 p % stride == offset % stride 的实例。
// 纯函数 + 顺序确定：同一份场景/表示每次得到同一结果（命令流重放才可能逐位一致）。
void buildInstanceRangeMask(const Representation& rep,
                            const std::vector<entity::MoleculeInstance>& instances,
                            std::vector<uint8_t>& out) {
    const uint8_t modeId = std::min<uint8_t>(
        rep.rangeMode,
        static_cast<uint8_t>(Representation::InstanceRangeMode::Stride));
    const Representation::InstanceRangeMode mode =
        static_cast<Representation::InstanceRangeMode>(modeId);
    const std::string wantType = trimToken(rep.rangeType);
    const bool typeFilter = !wantType.empty();

    // 1) 类型匹配：记录匹配实例的**场景下标**（匹配位置 = 该表中的顺序）
    std::vector<uint32_t> matched;
    matched.reserve(instances.size());
    for (uint32_t i = 0; i < instances.size(); ++i) {
        if (typeFilter && instanceTypeToken(instances[i].name) != wantType) {
            continue;
        }
        matched.push_back(i);
    }

    out.assign(instances.size(), 0u);
    if (matched.empty()) {
        return;   // 无匹配实例：全空（调用方据此不渲染任何实例）
    }

    switch (mode) {
        case Representation::InstanceRangeMode::Fraction: {
            const float f = std::isfinite(rep.rangeFraction)
                                ? std::clamp(rep.rangeFraction, 0.0f, 1.0f)
                                : 1.0f;
            // ceil(f*N)：0 → 0 个，1 → N 个；0.5 的 4 个 SOL → 前 2 个。
            // 用 double 计算避免 float 乘法在 f*N 恰为整数时向下误差（0.5*4=2.0）。
            const size_t keep = static_cast<size_t>(
                std::ceil(static_cast<double>(f) * static_cast<double>(matched.size())));
            for (size_t k = 0; k < keep && k < matched.size(); ++k) {
                out[matched[k]] = 1u;
            }
            break;
        }
        case Representation::InstanceRangeMode::Stride: {
            const uint32_t stride = std::max(1u, rep.rangeStride);
            const uint32_t offset = rep.rangeOffset % stride;
            for (size_t k = 0; k < matched.size(); ++k) {
                if ((static_cast<uint32_t>(k) % stride) == offset) {
                    out[matched[k]] = 1u;
                }
            }
            break;
        }
        case Representation::InstanceRangeMode::All:
        default:
            for (uint32_t idx : matched) {
                out[idx] = 1u;
            }
            break;
    }
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    // 恢复样式层默认（同步生成），再停止后台工作线程：否则进程退出/下一个
    // Renderer 实例可能继承"异步已开"的全局开关（CLI/测试需要同步路径）。
    quickSurfSetAsyncEnabled(false);
    // 等待/停止后台表面工作线程：否则进程退出时工作线程可能正在写已析构的样式层
    // 缓存（quicksurf 的静态网格缓存）。
    async::shutdownSurfaceJobs();
}

void Renderer::bindRepresentations(const std::vector<Representation>* reps,
                                   uint64_t revision) {
    if (representations_ != reps || repsRevision_ != revision) {
        representations_ = reps;
        repsRevision_ = revision;
        needsRebuild_ = true;
    }
}

void Renderer::unbindRepresentations() {
    representations_ = nullptr;
    repsRevision_ = 0;
}

void Renderer::setPaneFilter(std::vector<uint8_t> instanceMask,
                             std::vector<uint8_t> repMask) {
    if (paneInstanceMask_ == instanceMask && paneRepMask_ == repMask) {
        return;
    }
    paneInstanceMask_ = std::move(instanceMask);
    paneRepMask_ = std::move(repMask);
    std::lock_guard<std::mutex> prepareLock(prepareMutex_);
    // 筛选只影响命令流内容（不改场景数据）；必须立即重建，不能等 DIRTY_*。
    needsRebuild_ = true;
    needsCoords_ = false;
}

const std::vector<Representation>& Renderer::representations() const {
    static const std::vector<Representation> kNone;
    return representations_ ? *representations_ : kNone;
}

void Renderer::refreshFrameData() {
    // 帧数据：坐标段直接引用 CoordsSoA（世界坐标）
    if (scene_) {
        frameData_.positions = &scene_->coordsSoA().positions;
        frameData_.timestep = nullptr;
    }
}

void Renderer::update(float dt) {
    if (scene_ == nullptr) {
        return;
    }
    updateWithDirty(dt, scene_->dirtyBits());
}

void Renderer::updateWithDirty(float dt, uint32_t dirty) {
    (void)dt;
    if (scene_ == nullptr) {
        return;
    }
    // 与 prepare 共用锁：导出线程可能同时调用 update/prepare（见 header）；
    // 串行化后两条路径不会交错改写 needsRebuild_/commandList_。
    std::lock_guard<std::mutex> prepareLock(prepareMutex_);
    // 诊断（仅在脏位**取值变化**时打印，避免拖拽相机时逐帧刷屏）：
    // 本行不出现 = MolScene 的门控从未打开（它看到的 sceneDirty 恒为假）；
    // 出现且 dirty 非 0 = 门控打开了且脏位已传到渲染器 —— 那问题就在 prepare。
    {
        static uint32_t sLastDirty = UINT32_MAX;
        if (dirty != sLastDirty) {
            sLastDirty = dirty;
            spdlog::debug("[render] update: dirty=0x{:x} atoms={}", dirty,
                          scene_->coordsSoA().positions.size());
        }
    }
    if (dirty == 0) {
        return;
    }
    // 结构/实例/表示变更 → 拓扑可能变了，必须整条重建（坐标重放不适用）
    constexpr uint32_t kStructural =
        entity::DIRTY_STRUCTURE | entity::DIRTY_INSTANCE | entity::DIRTY_REPRESENTATION;
    if ((dirty & kStructural) != 0) {
        needsRebuild_ = true;
        needsCoords_ = false;
    } else if ((dirty & entity::DIRTY_COORDS) != 0) {
        // 仅帧切：命令序列不变，坐标变了 → 走重放（失败则回退整条重建）
        needsCoords_ = true;
    } else {
        // 只落到这里才是"仅 DIRTY_PROPERTIES"（背景/光照/雾/全局缩放）：只进
        // GlobalUBO，命令流无需重建 —— 没有任何下游会消费该脏位，故在此清除，
        // 否则门控会永久打开、视口逐帧空转重绘。
        scene_->clearDirty();
    }
    // ⚠ 结构/坐标脏位**不在此清除**：MolScene 的重绘门控读的就是 dirtyBits()
    //（见 MolScene::render），在这里提前清掉会让"打开文件后的首帧"看到
    // sceneDirty=false → 门控全假 → 视口永不重绘（表现为界面正常但视口空白，
    // 且命令流 serial 不再增长）。清脏统一由真正完成重建的 rebuildCommandList()
    // 末尾负责 —— 即"谁消费，谁清除"。
}

void Renderer::prepare() {
    if (scene_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> prepareLock(prepareMutex_);
    lastWasReplay_ = false;

    // 后台表面任务完成 → 需要重建一次把新网格换入（重建时命中样式层缓存，不再阻塞）。
    // 若仍在等待：直接返回（旧命令流保留，旧画面继续显示），不再逐帧尝试重建。
    // 运动中冻结表面时**不等待**后台任务：结果留给解冻后的重建，当前帧继续用
    // 冻结网格 + 坐标 replay —— 否则 700 ms/帧的表面生成会把播放拖到个位数 FPS。
    if (waitingForSurface_.load(std::memory_order_relaxed)) {
        if (freezeSurfaces_.load(std::memory_order_relaxed)) {
            async::consumeSurfaceJobCompletion();   // 丢弃通知（任务仍会写缓存）
            waitingForSurface_.store(false, std::memory_order_relaxed);
        } else {
            if (!async::consumeSurfaceJobCompletion() && async::surfaceJobsBusy()) {
                return;
            }
            waitingForSurface_.store(false, std::memory_order_relaxed);
            needsRebuild_ = true;
            // 可观测性：异步表面从提交到就绪的墙钟时间（含等待期 UI 保持响应的时长）
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - surfaceWaitStart_)
                                  .count();
            spdlog::debug("[render] 异步表面就绪：后台生成 {:.0f} ms（主线程期间未被阻塞），"
                          "重建命令流换入新表面",
                          ms);
        }
    } else if (asyncSurface_.load(std::memory_order_relaxed) &&
               async::consumeSurfaceJobCompletion()) {
        // 异步模式下的任务在别处完成（极少见：刚打开异步模式）→ 重建换入
        if (!freezeSurfaces_.load(std::memory_order_relaxed)) {
            needsRebuild_ = true;
        }
    }

    if (needsRebuild_) {
        const bool updated = rebuildCommandList(/*replay=*/false);
        needsRebuild_ = !updated;
        needsCoords_ = false;
        if (!updated) {
            // 有表面在后台生成：保留旧命令流，等完成通知后再次重建
            waitingForSurface_.store(true, std::memory_order_relaxed);
            surfaceWaitStart_ = std::chrono::steady_clock::now();
        }
        return;
    }
    if (!needsCoords_) {
        return;
    }
    needsCoords_ = false;
    const bool freezeNow = freezeSurfaces_.load(std::memory_order_relaxed);
    // QuickSurf 逐帧预取：异步模式下不冻结表面，让每帧重建命中预取缓存并换入
    // 当前帧网格（否则重放会一直保留旧表面，预取等于白算）。其它表面仍可冻结。
    const bool liveQuickSurf = !forceSyncRebuild_ &&
                               asyncSurface_.load(std::memory_order_relaxed) &&
                               sceneHasVisibleQuickSurf(representations());
    const bool freezeQuickNow = freezeNow && !liveQuickSurf;
    // 只剩冻结的整体系表面时，帧切不改变任何可见命令：保留原命令流与 serial。
    // 否则会每帧全量重建/重传 10 万+ 三角面的表面网格（实测 Debug 下 14 FPS）。
    if (!forceSyncRebuild_ &&
        !sceneNeedsCoordUpdate(representations(), freezeNow, freezeQuickNow)) {
        lastWasReplay_ = true;
        scene_->clearDirty();
        return;
    }
    // 帧切快速路径：只把坐标类命令的载荷写回原偏移（不重建命令序列）。
    // 计划不可用（含 TriMesh / 尚未生成）或重放中途失配 → 回退整条重建。
    const bool allowMeshReplay =
        sceneMeshReplaySafe(representations(), freezeNow, freezeQuickNow);
    if (commandList_.beginReplay(allowMeshReplay)) {
        rebuildCommandList(/*replay=*/true);
        if (commandList_.endReplay()) {
            lastWasReplay_ = true;
            // 重放路径在 rebuildCommandList 里提前 return，不会走到那里的 clearDirty；
            // 而本次重放已消费了坐标脏位 → 在此清除，否则门控永久打开、逐帧空转重放。
            scene_->clearDirty();
            return;
        }
        spdlog::debug("[render] coord replay mismatch -> full rebuild");
    }
    const bool updated = rebuildCommandList(/*replay=*/false);
    if (!updated) {
        needsRebuild_ = true;
        waitingForSurface_.store(true, std::memory_order_relaxed);
    }
}

void Renderer::setAsyncSurfaceEnabled(bool enabled) {
    asyncSurface_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        // 关闭异步：进行中的任务保留（结果仍会写进缓存），但不再"等结果再重建"。
        // 下一轮 prepare 走同步路径，本次最多阻塞一次生成。
        waitingForSurface_.store(false, std::memory_order_relaxed);
    }
}

void Renderer::setSurfaceFreeze(bool frozen) {
    const bool was = freezeSurfaces_.exchange(frozen, std::memory_order_relaxed);
    if (was == frozen) {
        return;
    }
    if (!frozen) {
        // 解冻：让下一次 prepare 重建一次，把表面刷到当前帧。
        std::lock_guard<std::mutex> lk(prepareMutex_);
        needsRebuild_ = true;
    }
}

bool Renderer::surfaceGenerationPending() const {
    if (freezeSurfaces_.load(std::memory_order_relaxed)) {
        return false;   // 冻结期间不显示"生成中"（表面不会在本帧更新）
    }
    return waitingForSurface_.load(std::memory_order_relaxed) ||
           async::surfaceJobsBusy();
}

void Renderer::prepareBlocking() {
    if (scene_ == nullptr) {
        return;
    }
    // 导出线程专用：先等后台表面任务把网格写进缓存（不持锁等，工作线程无需 prepare 锁），
    // 再强制同步重建一次 —— 导出图不会出现"生成中"的旧画面。
    const bool surfacePending = waitingForSurface_.load(std::memory_order_relaxed) ||
                                async::surfaceJobsBusy();
    if (surfacePending) {
        async::waitForSurfaceJobs();
    }
    std::lock_guard<std::mutex> prepareLock(prepareMutex_);
    async::consumeSurfaceJobCompletion();
    if (surfacePending) {
        waitingForSurface_.store(false, std::memory_order_relaxed);
        needsRebuild_ = true;
    }
    forceSyncRebuild_ = true;
    lastWasReplay_ = false;
    if (needsRebuild_) {
        rebuildCommandList(/*replay=*/false);
        needsRebuild_ = false;
        needsCoords_ = false;
    } else if (needsCoords_) {
        needsCoords_ = false;
        if (commandList_.beginReplay(sceneMeshReplaySafe(
                representations(), false, false))) {
            rebuildCommandList(/*replay=*/true);
            if (commandList_.endReplay()) {
                lastWasReplay_ = true;
                scene_->clearDirty();
                forceSyncRebuild_ = false;
                return;
            }
        }
        rebuildCommandList(/*replay=*/false);
        needsRebuild_ = false;
        needsCoords_ = false;
    }
    forceSyncRebuild_ = false;
}

bool Renderer::rebuildCommandList(bool replay) {
    refreshFrameData();

    // 样式层异步开关：只有"GUI 异步模式 + 非强制同步 + 非重放"才允许整体系表面
    // 提交后台任务；其余路径（CLI/导出/测试/重放）全部同步生成，行为与既有一致。
    quickSurfSetAsyncEnabled(asyncSurface_.load(std::memory_order_relaxed) &&
                             !forceSyncRebuild_ && !replay);
    // 运动中冻结坐标相关表面：导出（forceSyncRebuild_）必须拿到当前帧的表面，
    // 因此导出路径不冻结；其余（播放/拖时间轴/坐标重放）冻结。
    const bool freeze = freezeSurfaces_.load(std::memory_order_relaxed) &&
                        !forceSyncRebuild_;
    const bool liveQuickSurf = !forceSyncRebuild_ &&
                               asyncSurface_.load(std::memory_order_relaxed) &&
                               sceneHasVisibleQuickSurf(representations());
    quickSurfSetFreezeGeometry(freeze && !liveQuickSurf);
    surfaceSetFreezeGeometry(freeze);

    // 全量重建：清空上一轮的 ESP 视口叠加信息（色标条/极值标注），
    // 由本次重建中的 drawEspSurface 重新写入；坐标重放路径不重建样式，故不清。
    if (!replay) {
        espOverlayReset();
    }

    // 全量重建：生成到临时列表，成功后再整体换入（生成中途失败不留半个命令流），
    // 同时边生成边记录坐标命令偏移（供后续帧切重放）。
    // 重放：直接写回现列表的坐标载荷，不增长池、不重发状态命令。
    SceneCommandList local;
    SceneCommandList& fresh = replay ? commandList_ : local;
    if (!replay) {
        fresh.recordReplay(true);
    }
    // 本轮是否提交过后台表面任务：提交则本轮命令流不完整（缺该表面），不换入。
    const uint64_t submittedBefore = async::submittedSurfaceJobCount();

    // 逐实例起始原子下标（前缀和，O(n) 一次）：渲染循环里每个实例都会用到，
    // 若每次现算就是 O(n²)。md.gro（2042 实例）坐标重放时这是主要 CPU 开销之一。
    std::vector<uint32_t> atomBases(scene_->instances().size() + 1, 0u);
    {
        const auto& pool = scene_->structures();
        const auto& insts = scene_->instances();
        for (size_t i = 0; i < insts.size(); ++i) {
            const size_t n = insts[i].structureIndex < pool.size()
                                 ? pool[insts[i].structureIndex].elements.size()
                                 : 0;
            atomBases[i + 1] = atomBases[i] + static_cast<uint32_t>(n);
        }
    }

    // 每 (表示, 实例) 生成段（P10 并行；此处串行实现，M2 接入 TaskPool）——
    // Selection 掩码对整个场景求值一次（与实例无关），本表示全部实例复用。
    const std::vector<Representation>& reps = representations();
    for (size_t repIdx = 0; repIdx < reps.size(); ++repIdx) {
        const Representation& rep = reps[repIdx];
        if (!rep.visible) {
            continue;
        }
        // 多视图：该视图没有勾选这条表示 → 不生成它的任何命令（重放路径同样跳过，
        // 否则命令序列与全量重建对不上，重放游标会错位）。
        if (!paneRepMask_.empty() &&
            (repIdx >= paneRepMask_.size() || paneRepMask_[repIdx] == 0)) {
            continue;
        }
        // Selection 求值（跨实例全局掩码）
        std::vector<bool> mask;
        if (rep.selection.empty()) {
            mask.assign(scene_->coordsSoA().atomIds.size(), true);
        } else {
            entity::Selection sel;
            const entity::ParseTable& table = entity::ParseTable::defaultTable();
            if (sel.parse(rep.selection, table)) {
                sel.evaluate(*scene_, frameData_, 0);
                mask = sel.mask();
            } else {
                mask.assign(scene_->coordsSoA().atomIds.size(), true);
            }
        }

        // 实例级类型范围（v2.19）：每条表示只算一次，逐实例与整体系表面共用同一份
        // 结果（"只渲染一半溶剂"必须对两条路径一致生效）。
        std::vector<uint8_t> instanceRangeMask;
        buildInstanceRangeMask(rep, scene_->instances(), instanceRangeMask);

        // 多视图：再叠加本视图的实例筛选（文件/体系）。两者都通过才渲染；
        // 整体系表面走同一份掩码 → 被筛掉的体系不会混进该视图的表面。
        // 视图掩码之外的实例一律视为隐藏（掩码按当前场景实例数生成，正常等长）。
        if (!paneInstanceMask_.empty()) {
            const size_t n = instanceRangeMask.size();
            for (size_t i = 0; i < n; ++i) {
                const bool keep = i < paneInstanceMask_.size() &&
                                  paneInstanceMask_[i] != 0;
                instanceRangeMask[i] = (instanceRangeMask[i] != 0 && keep) ? 1u : 0u;
            }
        }

        // 体系级表面（G5）：整体系（含溶剂分子）一个整体表面，只发射一次（而非
        // 每分子一个独立小表面）。surface/quicksurf 表示设置 rep.systemSurface 时生效。
        // 该分支发射 TriMesh（顶点数随坐标变化）→ 记录期即置 hasMesh_，重放被拒，
        // 故重放模式下不会走到这里（断言以防将来改动破坏该前提）。
        if (rep.systemSurface &&
            (rep.type == RepresentationType::Surface ||
             rep.type == RepresentationType::QuickSurf)) {
            // 冻结重放：整体系表面 TriMesh 保留在旧命令池中，不重新发射；但重放
            // 计划里仍记录着它的 TriMesh 条目，必须连续消费掉，否则下一条球棍
            // 坐标命令会与 TriMesh 条目错位 → endReplay 失败并回退全量重建。
            if (replay) {
                commandList_.skipReplayMesh();
                continue;
            }
            // 整体表面是单一 TriMesh；必须先重置 RenderMode 与材质：若此前其它表示的
            // 最后一条 RenderMode/MaterialBlock 命令残留，表面网格会继承错误的管线
            // （线框/透明）或全零默认材质（diffuse=0）→ 烘焙后顶点色全黑 → 表面不可见。
            // v2.19：重置值改由表示自己的 surfaceDrawMode 决定（0 实心/1 线框/2 半透明），
            // 否则用户永远拿不到"表面绘制模式"这个开关。
            RenderModeInst rmi0;
            rmi0.mode = std::min<uint8_t>(rep.surfaceDrawMode, 2u);
            rmi0.pad[0] = rmi0.pad[1] = rmi0.pad[2] = 0;
            fresh.append(Cmd::RenderMode, &rmi0, sizeof(rmi0));
            MaterialBlock mat0;
            mat0.ambient = glm::vec4(rep.ambient[0], rep.ambient[1], rep.ambient[2],
                                     rep.ambient[3]);
            mat0.diffuse = glm::vec4(rep.diffuse[0], rep.diffuse[1], rep.diffuse[2],
                                     rep.diffuse[3]);
            mat0.specular = glm::vec4(rep.specular[0], rep.specular[1], rep.specular[2],
                                      rep.specular[3]);
            mat0.shininess = rep.shininess;
            mat0.metallic = rep.metallic;
            mat0.roughness = rep.roughness;
            mat0.opacity = rep.opacity;
            fresh.append(Cmd::MaterialBlock, &mat0, sizeof(mat0));

            const auto& surfInstances = scene_->instances();
            RepContext sctx;
            sctx.scene = scene_;
            sctx.frame = &frameData_;
            sctx.rep = &rep;
            sctx.mask = &mask;
            sctx.instanceMask = &instanceRangeMask;
            // 整体系表面没有"单一实例"，但样式层历史上有读 ctx.instance 的路径
            // （colorForAtom 空指针解引用崩溃）；防御性地指向首个实例。取第一个
            // 未被类型范围过滤的实例优先，全被过滤时退回第 0 个（不作为取色依据）。
            if (!surfInstances.empty()) {
                uint32_t first = 0;
                for (uint32_t k = 0; k < surfInstances.size(); ++k) {
                    if (k < instanceRangeMask.size() && instanceRangeMask[k] != 0) {
                        first = k;
                        break;
                    }
                }
                sctx.instance = &surfInstances[first];
                sctx.instanceIndex = first;
            }
            sctx.frameIndex = frameIndex_;
            if (rep.type == RepresentationType::Surface) {
                drawSystemSurface(sctx, fresh);
            } else {
                drawSystemQuickSurf(sctx, fresh);
            }
            continue;   // 跳过逐实例生成（整体系一个表面已足够）
        }

        // 下标遍历（实例下标即所需参数；此前按引用做线性反查 → 每实例一次
        // O(n) 扫描，整体 O(n²)，大体系下是 prepare 的主要开销之一）
        const auto& instances = scene_->instances();
        for (uint32_t instIdx = 0; instIdx < instances.size(); ++instIdx) {
            const auto& inst = instances[instIdx];
            // 实例级类型范围（v2.19）：被过滤的实例不参与本表示。重放路径同样跳过
            // ——命令序列与全量重建必须逐条对应，否则重放游标错位会回退全量重建。
            if (instIdx < instanceRangeMask.size() && instanceRangeMask[instIdx] == 0) {
                continue;
            }
            if (!inst.visible) {
                continue;
            }
            // 体系显隐：整个体系隐藏则其成员整体不渲染（体系作为整体对象）
            if (inst.systemId != UINT32_MAX) {
                const uint32_t si = scene_->systemIndexById(inst.systemId);
                if (si != UINT32_MAX && !scene_->systems()[si].visible) {
                    continue;
                }
            }

            if (replay) {
                // 重放：跳过 Representer::generate（它会重发 RenderMode/Material
                // 状态命令，而重放计划里只有坐标命令），直接调样式的坐标数学。
                // 命令序列与全量重建逐条对应，append 由游标改道写回原偏移。
                RepContext ctx;
                ctx.scene = scene_;
                ctx.instance = &inst;
                ctx.frame = &frameData_;
                ctx.rep = &rep;
                ctx.mask = &mask;
                ctx.instanceMask = &instanceRangeMask;
                ctx.instanceIndex = instIdx;
                ctx.globalAtomBase = atomBases[instIdx];
                ctx.frameIndex = frameIndex_;
                ctx.replay = true;
                ctx.renderMode = 0;
                if (inst.systemId != UINT32_MAX) {
                    const uint32_t si = scene_->systemIndexById(inst.systemId);
                    if (si != UINT32_MAX) {
                        ctx.renderMode =
                            static_cast<uint8_t>(scene_->systems()[si].renderMode);
                    }
                }
                if (StyleDrawFn drawStyle = styleFor(rep.type); drawStyle != nullptr) {
                    drawStyle(ctx, fresh);
                }
            } else {
                Representer representer;
                representer.generate(rep, *scene_, inst, instIdx, frameData_, 0,
                                     mask, fresh, atomBases[instIdx]);
            }
        }
    }

    if (replay) {
        return true;   // 重放不换入列表、不 bumpSerial（由 endReplay 统一处理）
    }
    fresh.recordReplay(false);
    // 异步表面：本轮有后台任务在跑（新提交或仍在生成）→ 新命令流缺表面，
    // **丢弃它、保留旧命令流**（旧画面继续显示），等任务完成后由 prepare 再重建。
    // 注意不 clearDirty：让 MolScene 的重绘门控保持打开，任务完成即可重建。
    if (!forceSyncRebuild_ && !freeze && async::surfaceJobsBusy()) {
        return false;
    }
    if (async::submittedSurfaceJobCount() != submittedBefore) {
        return false;   // 极快完成的情况：结果已进缓存，下一轮重建直接命中
    }
    // ⚠ 先把旧 serial 搬到新列表再换入：fresh 是**新建的空列表**（serial_ = 0），
    // 直接 move 会把 serial 复位成 0，其后 bumpSerial 恒得 1 —— 于是每次全量重建
    // 的 serial 都是 1，而后端解码缓存以 (列表对象, serial) 为键 → 误判缓存命中，
    // 继续使用上一次（往往还是空场景的）绘制列表。症状：**打开文件后界面一切正常、
    // 视口却始终空白**，且重建日志里 serial 一直是 1（很难联想到缓存键碰撞）。
    // 单调递增是这条链路的硬不变量，显式搬运以免再被 move 语义破坏。
    fresh.setSerial(commandList_.serial());
    // 网格内容修订号：只有全量重建才变；坐标重放保持，后端据此避免每帧重传
    // 表面网格（多表示 + 冻结 QuickSurf 时的主要开销）。
    fresh.setMeshSerial(++meshSerialCounter_);
    commandList_ = std::move(fresh);
    commandList_.bumpSerial();   // serial 递增：视图层用其检测命令流重建（缓存键 P5）
    scene_->clearDirty();
    return true;
}

void Renderer::draw(const RenderView& view) {
    if (vulkan_ == nullptr) {
        return;
    }
    vulkan_->execute(commandList_, view);
}

LightParams Renderer::sceneLighting() const {
    LightParams light;
    if (!scene_) {
        return light;
    }
    // 半球环境光（天顶/地面色 + 均匀环境光）+ 方向光（主方案 §6.6.4）
    const auto& props = scene_->properties();
    light.skyColor = glm::vec3(props.skyColor[0], props.skyColor[1], props.skyColor[2]);
    light.groundColor =
        glm::vec3(props.groundColor[0], props.groundColor[1], props.groundColor[2]);
    light.ambientColor =
        glm::vec3(props.ambientColor[0], props.ambientColor[1], props.ambientColor[2]);
    light.ambientIntensity = props.ambient;
    light.dirLightDir =
        glm::vec3(props.dirLightDir[0], props.dirLightDir[1], props.dirLightDir[2]);
    light.dirLightColor =
        glm::vec3(props.dirLightColor[0], props.dirLightColor[1], props.dirLightColor[2]);
    light.dirLightIntensity = props.dirLightIntensity;
    light.dirLightEnabled = props.dirLightEnabled;
    light.shadowsEnabled = props.shadowsEnabled;
    light.shadowStrength = props.shadowStrength;

    // 方向光阴影的光源正交矩阵：按场景包围球拟合，使整场景都落在光源视锥内。
    // 用包围【球】而非包围盒的轴对齐结果：光源方向任意，球是方向无关的保守包围，
    // 不会出现"某些朝向被裁掉一半"。
    if (light.dirLightEnabled && light.dirLightIntensity > 0.0f) {
        glm::vec3 center(0.0f);
        float radius = 1.0f;
        if (sceneBounds(&center, &radius)) {
            const glm::vec3 l = glm::normalize(light.dirLightDir);
            // 光源"眼睛"放在球外（沿光线方向退开 2 倍半径），看向球心
            const glm::vec3 eye = center + l * (radius * 2.0f);
            // up 不能与视线平行：视线接近 ±Y 时改用 X 轴
            const glm::vec3 up =
                std::abs(l.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::mat4 view = glm::lookAt(eye, center, up);
            // 正交半宽 = 半径 ×1.1（留边）；近远覆盖 eye 到球心的距离 ± 半径
            const float half = radius * 1.1f;
            const float dist = radius * 2.0f;
            const glm::mat4 proj =
                glm::ortho(-half, half, -half, half, 0.01f, dist + radius * 2.0f);
            light.lightViewProj = proj * view;
        }
    }
    return light;
}

glm::vec4 Renderer::sceneFog() const {
    if (!scene_) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 0.05f);   // 与 RenderView 默认一致（雾关）
    }
    const auto& props = scene_->properties();
    return glm::vec4(props.fog[0], props.fog[1], props.fogMode, props.fogDensity);
}

RenderView Renderer::viewFor(const OrbitCamera& camera, uint32_t width,
                             uint32_t height) const {
    RenderView view;
    view.camera = camera;
    view.width = std::max(1u, width);
    view.height = std::max(1u, height);
    view.light = sceneLighting();
    view.style = style_;
    if (scene_ != nullptr) {
        const auto& bg = scene_->properties().bgColor;
        view.backgroundColor = glm::vec3(bg[0], bg[1], bg[2]);
        const auto& props = scene_->properties();
        view.clipEnabled = props.clipEnabled;
        view.clipPlane = glm::vec4(props.clipPlane[0], props.clipPlane[1],
                                   props.clipPlane[2], props.clipPlane[3]);
    }
    view.fog = sceneFog();
    view.scale = scene_ != nullptr ? scene_->properties().scale : 1.0f;
    return view;
}

bool Renderer::sceneBounds(glm::vec3* outCenter, float* outRadius) const {
    if (scene_ == nullptr || scene_->coordsSoA().positions.empty()) {
        return false;
    }
    const auto& pos = scene_->coordsSoA().positions;
    glm::vec3 lo = pos[0], hi = pos[0];
    for (const auto& p : pos) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    *outCenter = (lo + hi) * 0.5f;
    float radius = glm::length(hi - lo) * 0.5f;
    // 半径下限 = 所有原子 vdW 半径的最大值（单原子/全部重叠时包围球退化为
    // 点，radius=0 → centerOn 距离/近远面退化 → 相机贴在原子内 → 不可见）。
    // 取最大 vdW 半径保证相机距离合理，单原子在各渲染模式（VDW/表面/球棍…）
    // 都能正确显示。
    float minRadius = 1.0f;
    for (const auto& ms : scene_->structures()) {
        for (const auto el : ms.elements) {
            minRadius = std::max(minRadius, entity::elementInfo(el).vdwRadius);
        }
    }
    *outRadius = std::max(radius, minRadius);
    return true;
}

// —— GPU 光追（懒初始化；无 RT 设备返回 false，调用方回退 CPU）——
bool Renderer::gpuRayTraceAvailable() {
    std::lock_guard<std::mutex> lock(gpuRtMutex_);
    if (!gpuRt_) {
        gpuRt_ = std::make_unique<GpuRayTracer>();
    }
    if (gpuRt_->available()) {
        return true;
    }
    // 有主设备（GUI）就复用；没有（CLI/测试）则由 GpuRayTracer 自建 headless。
    VulkanDevice* dev = (vulkan_ != nullptr) ? &vulkan_->device() : nullptr;
    std::string err;
    const bool ok = gpuRt_->init(dev, &err);
    if (!ok) {
        spdlog::info("[rt] GPU ray tracing unavailable: {}", err);
    }
    return ok;
}

bool Renderer::gpuRayTrace(const RtScene& scene, const RtCamera& camera,
                           const RtLight& light, const glm::vec3& backgroundColor,
                           int samplesPerPixel, std::vector<uint8_t>& outRgba) {
    std::lock_guard<std::mutex> lock(gpuRtMutex_);
    if (!gpuRt_) {
        gpuRt_ = std::make_unique<GpuRayTracer>();
    }
    if (!gpuRt_->available()) {
        VulkanDevice* dev = (vulkan_ != nullptr) ? &vulkan_->device() : nullptr;
        if (!gpuRt_->init(dev, nullptr)) {
            return false;
        }
    }
    return gpuRt_->render(scene, camera, light, backgroundColor, samplesPerPixel,
                          outRgba);
}

} // namespace moleculens::engine
