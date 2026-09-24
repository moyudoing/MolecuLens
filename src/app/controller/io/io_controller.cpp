// =============================================================================
// MolecuLens · app · controller/io/io_controller.cpp —— 文件 IO 实现
// 规格来源：主方案 §5.8.1（加载 → 显示 静态全流程 ①②③）
// =============================================================================

#include "app/controller/io/io_controller.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

#include "entity/trajectory/dcd_reader.h"

#include "app/controller/rep/representation_controller.h"
#include "engine/camera/camera.h"
#include "engine/render/renderer.h"
#include "engine/serialize/scene_bridge.h"
#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"
#include "persist/mlscene/scene_serializer.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

// 源文件指纹（轨迹以"引用 + 指纹"写入 .mlscene；文件被替换时据此提示重新加载）
void fileFingerprint(const std::filesystem::path& p, uint64_t* size, int64_t* mtime) {
    std::error_code ec;
    *size = std::filesystem::file_size(p, ec);
    if (ec) {
        *size = 0;
    }
    const auto t = std::filesystem::last_write_time(p, ec);
    *mtime = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

// 解析器返回的是独立 SceneStructure（自带 StringPool/链/残基表），
// 这里把其中的结构与实例装配进应用场景：原子名重 intern、链/残基 id 重映射
// （否则 id 指向解析器私有表 → 选择语言/名称查询错误）、结构去重注册、实例追加。
// 一次导入 = 一个体系：整个体系（合并包围盒）居中于原点，统一所有导入体系的
// 初始位置（体系间初始完全重叠，用户用移动/定位工具分开，见 moleculens-fixes）。
void mergeParsedScene(const entity::SceneStructure& src,
                      entity::SceneStructure& dst,
                      const std::string& systemName) {
    // 链/残基名 → dst id 反向映射（复用既有，新增追加）
    std::unordered_map<std::string, int32_t> chainIds, resIds;
    for (int32_t i = 0; i < dst.chainCount(); ++i) {
        chainIds.emplace(std::string(dst.chainName(i)), i);
    }
    for (int32_t i = 0; i < dst.residueCount(); ++i) {
        resIds.emplace(std::string(dst.residueName(i)), i);
    }

    // 结构下标映射：src 下标 → dst 下标（registerStructure 去重）
    std::vector<uint32_t> structMap(src.structures().size());
    for (size_t si = 0; si < src.structures().size(); ++si) {
        const entity::MoleculeStructure& srcMs = src.structures()[si];
        entity::MoleculeStructure ms = srcMs;
        const uint32_t srcSi = static_cast<uint32_t>(si);
        for (size_t ai = 0; ai < ms.atoms.size(); ++ai) {
            // 链 id 重映射
            const int32_t srcChain = srcMs.atoms[ai].chainId;
            if (srcChain >= 0) {
                const std::string cname(src.chainName(srcChain));
                auto it = chainIds.find(cname);
                if (it == chainIds.end()) {
                    it = chainIds.emplace(cname, dst.addChain(cname)).first;
                }
                ms.atoms[ai].chainId = it->second;
            }
            // 残基 id 重映射
            const int32_t srcRes = srcMs.atoms[ai].residueId;
            if (srcRes >= 0) {
                const std::string rname(src.residueName(srcRes));
                auto it = resIds.find(rname);
                if (it == resIds.end()) {
                    it = resIds.emplace(rname, dst.addResidue(rname)).first;
                }
                ms.atoms[ai].residueId = it->second;
            }
            // 原子名重 intern 到 dst 的 StringPool
            if (ai < ms.atomNameIds.size()) {
                ms.atomNameIds[ai] =
                    dst.strings().intern(src.atomName(srcSi, static_cast<uint32_t>(ai)));
            }
        }
        structMap[si] = dst.registerStructure(ms);
    }

    // 一次导入 = 一个体系：建体系后，把本次导入全部分子的合并几何中心平移到
    // 体系原点（0,0,0）——所有导入体系的初始位置保持一致（体系居中于原点）。
    const uint32_t sysId = dst.addSystem(systemName);
    glm::vec3 center(0.0f);
    size_t pointCount = 0;
    for (const auto& inst : src.instances()) {
        if (inst.trajectory && !inst.trajectory->frames.empty()) {
            for (const auto& p : inst.trajectory->frames[0].positions) {
                center += p;
                ++pointCount;
            }
        }
    }
    if (pointCount > 0) {
        center /= static_cast<float>(pointCount);
    }

    for (const entity::MoleculeInstance& inst : src.instances()) {
        if (inst.structureIndex >= structMap.size()) {
            continue;
        }
        entity::Transform tm = inst.transform;
        tm.translation -= center;   // 体系中心 → 原点（体系内分子相对排布保留）
        // 保留实例名与溶剂标记（gro 按残基拆分子：名 = 残基名#序号、isSolvent 标记）
        dst.addTrajectoryInstance(structMap[inst.structureIndex], tm, inst.trajectory,
                                  sysId, inst.name, inst.isSolvent);
    }
}

} // namespace

IoController::IoController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

IoController::~IoController() {
    // 后台解析线程 join（应用关闭时确保线程结束，避免访问已析构成员）
    if (parseThread_.joinable()) {
        parseThread_.join();
    }
}

bool IoController::openFile(const std::filesystem::path& path) {
    auto report = [&](int stage, float progress, const char* name) {
        if (loadCb_) {
            LoadProgress p;
            p.stage = stage;
            p.progress = progress;
            p.fileName = name ? name : path.filename().string();
            loadCb_(p);
        }
    };
    report(1, -1.0f, nullptr);   // ReadingFile（排队；实际解析在后台线程）

    // .mlscene 是"整场景替换"语义（clear + 还原），不是解析某种分子格式，
    // 故不走 ParserRegistry 与后台解析线程，同步载入。
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".mlscene") {
        if (parseThread_.joinable()) {
            parseThread_.join();
        }
        const bool ok = openSceneFile(path);
        report(ok ? 4 : 5, ok ? 1.0f : 0.0f, nullptr);
        return ok;
    }

    // 记录源文件指纹（供 saveScene 写入轨迹引用）
    currentSourcePath_ = path.string();
    fileFingerprint(path, &currentSourceSize_, &currentSourceMtime_);

    // 等待上一次后台解析线程结束（同一时刻只允许一次导入）
    if (parseThread_.joinable()) {
        parseThread_.join();
    }
    // QS.5 文件导入多线程：解析放后台线程，避免大文件解析卡住主视图。
    // 解析完成后 parsedReady_=true；主线程 consumeParsed（runLoop 每帧）再合并进场景。
    parseThread_ = std::thread([this, path]() {
        auto result = entity::ParserRegistry::defaultRegistry().parseFile(
            path, entity::ParseOptions{});
        std::shared_ptr<entity::SceneStructure> scene;
        std::string err;
        if (result) {
            scene = result->scene;
        } else {
            err = result.errorMessage();
            spdlog::error("[io] load failed (background): {}", err);
        }
        {
            std::lock_guard<std::mutex> lk(parsedMutex_);
            parsedScene_ = std::move(scene);
            parsedError_ = std::move(err);
            parsedName_ = path.stem().string();
        }
        parsedReady_ = true;
    });
    return true;
}

// 主线程每帧调用：后台解析完成 → 合并进场景（渲染器读场景须在主线程）
void IoController::consumeParsed() {
    if (!parsedReady_.exchange(false)) {
        return;
    }
    std::shared_ptr<entity::SceneStructure> parsed;
    std::string name;
    {
        std::lock_guard<std::mutex> lk(parsedMutex_);
        parsed = std::move(parsedScene_);
        name = parsedName_;
    }
    const auto report = [&](int stage, float progress) {
        if (loadCb_) {
            LoadProgress p;
            p.stage = stage;
            p.progress = progress;
            p.fileName = name;
            loadCb_(p);
        }
    };
    if (!parsed) {
        report(5, 0.0f);   // Failed
        return;
    }
    report(2, 0.4f);   // AnalyzingBonds
    mergeParsed(std::move(parsed), name);
    report(3, 0.8f);   // UploadingGPU
    report(4, 1.0f);   // Done
}

// 合并解析结果进应用场景 + 渲染器刷新（主线程调用）
void IoController::mergeParsed(std::shared_ptr<entity::SceneStructure> parsed,
                               const std::string& name) {
    if (svc_.scene && parsed) {
        mergeParsedScene(*parsed, *svc_.scene, name);
        svc_.scene->setDirty(entity::DIRTY_ALL);
        svc_.scene->bumpSerial();
    }
    if (svc_.renderer) {
        // 表示数据由 RepresentationController 唯一持有并已绑定（零拷贝只读视图），
        // 此处不再回写表示列表——只需刷新帧数据并置脏（主方案 §5.8.1）。
        svc_.renderer->setScene(svc_.scene);
        svc_.renderer->refreshFrameData();
        if (svc_.scene) {
            svc_.scene->setDirty(entity::DIRTY_ALL);
        }
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::SceneOpened, 0});
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
}

bool IoController::saveScene(const std::filesystem::path& path) {
    (void)path;
    if (svc_.scene == nullptr) {
        spdlog::error("[io] saveScene: 场景未就绪");
        return false;
    }
    // 场景 → 快照：表示取自渲染器绑定的只读视图（其唯一持有者是表示控制器），
    // 相机取自 AppServices，命令流由桥接层用 SceneCommandList::writeTo 序列化。
    engine::SnapshotSource in;
    in.scene = svc_.scene;
    in.representations = svc_.renderer ? &svc_.renderer->representations() : nullptr;
    in.camera = svc_.camera;
    in.commandStream = svc_.renderer ? &svc_.renderer->commandList() : nullptr;
    in.trajectorySourcePath = currentSourcePath_;
    in.trajectorySourceSize = currentSourceSize_;
    in.trajectorySourceMtime = currentSourceMtime_;

    const auto snap = engine::makeSnapshot(in);
    auto result = persist::mlscene::MlSceneSerializer::save(snap, path);
    if (!result) {
        spdlog::error("[io] saveScene 失败: {}", result.errorMessage());
        return false;
    }
    spdlog::info("[io] 场景已保存: {} ({} 结构 / {} 体系 / {} 实例)", path.string(),
                 snap.structures.size(), snap.systems.size(), snap.instances.size());
    return true;
}

// 导入轨迹：读取器 → 逐帧读入内存 → 挂到目标实例 → 广播 SceneOpened 让时间轴激活
bool IoController::importTrajectory(const std::filesystem::path& path) {
    if (svc_.scene == nullptr) {
        return false;
    }
    // 读取器分派（按扩展名；当前内建 DCD）
    std::unique_ptr<entity::ITrajectoryReader> reader;
    {
        entity::DcdReader probe;
        if (probe.canOpen(path)) {
            reader = std::make_unique<entity::DcdReader>();
        }
    }
    if (!reader) {
        spdlog::error("[io] 不支持的轨迹格式（当前支持 .dcd）: {}", path.string());
        return false;
    }
    auto opened = reader->open(path);
    if (!opened) {
        spdlog::error("[io] 轨迹打开失败: {}", opened.errorMessage());
        return false;
    }

    const auto& insts = svc_.scene->instances();
    if (insts.empty()) {
        spdlog::error("[io] 请先打开结构文件，再导入轨迹（轨迹文件不含拓扑）");
        return false;
    }
    // 目标实例：当前体系的首个实例；无体系归属则取第一个实例
    uint32_t target = 0;
    const uint32_t cur = svc_.scene->currentSystemId();
    for (uint32_t i = 0; i < insts.size(); ++i) {
        if (insts[i].systemId == cur) {
            target = i;
            break;
        }
    }
    const auto& st = svc_.scene->structures()[insts[target].structureIndex];
    if (reader->atomCount() != st.elements.size()) {
        spdlog::error("[io] 轨迹原子数({}) 与目标结构原子数({}) 不一致 —— "
                      "DCD 不含拓扑，须与同源结构文件（PDB/GRO）配对导入",
                      reader->atomCount(), st.elements.size());
        return false;
    }

    // 逐帧读入内存。轨迹文件通常数十 MB（1000 帧 × 6000 原子 ≈ 72 MB），
    // 常驻内存换取实现简单；后续若需超长轨迹可改为按需读取（见 ITrajectoryReader）。
    auto traj = std::make_shared<entity::Trajectory>();
    traj->dt = reader->dt();
    traj->frames.reserve(static_cast<size_t>(reader->frameCount()));
    for (uint64_t f = 0; f < reader->frameCount(); ++f) {
        auto fr = reader->readFrame(f);
        if (!fr) {
            break;
        }
        traj->frames.push_back(std::move(fr.value()));
    }
    if (traj->frames.empty()) {
        spdlog::error("[io] 轨迹不含有效帧");
        return false;
    }

    if (!svc_.scene->setInstanceTrajectory(target, traj)) {
        return false;
    }
    // 广播 SceneOpened：时间轴据此重新激活并发布首帧（帧坐标 → 世界坐标的
    // 变换由时间轴统一负责，此处不重复实现）
    if (svc_.bus) {
        svc_.bus->publish({EventType::SceneOpened, 0});
    }
    spdlog::info("[io] 轨迹已导入: {} → 实例 {} ({} 帧, dt={} ps)", path.string(),
                 target, traj->frames.size(), traj->dt);
    return true;
}

// .mlscene 同步载入：整场景替换（clear → 还原 → 刷渲染器 → 事件）
bool IoController::openSceneFile(const std::filesystem::path& path) {
    auto loaded = persist::mlscene::MlSceneSerializer::load(path);
    if (!loaded) {
        spdlog::error("[io] 场景载入失败: {}", loaded.errorMessage());
        return false;
    }
    if (svc_.scene == nullptr) {
        return false;
    }
    const auto& snap = loaded.value();
    svc_.scene->clear();

    engine::RestoreTarget target;
    target.scene = svc_.scene;
    target.camera = svc_.camera;
    target.commandStream = svc_.renderer ? &svc_.renderer->commandList() : nullptr;
    std::vector<engine::Representation> reps;
    target.representations = &reps;
    engine::applySnapshot(snap, target);

    if (repCtl_ != nullptr && !reps.empty()) {
        repCtl_->replaceAll(std::move(reps));   // 表示的唯一持有者：由它换入并重新绑定
    }

    // 轨迹源引用（帧数据未内嵌，重放需按需加载 —— 见 P4 轨迹导入）
    if (!snap.instances.empty()) {
        currentSourcePath_ = snap.instances[0].trajectory.sourcePath;
        currentSourceSize_ = snap.instances[0].trajectory.sourceSize;
        currentSourceMtime_ = snap.instances[0].trajectory.sourceMtime;
    } else {
        currentSourcePath_.clear();
        currentSourceSize_ = 0;
        currentSourceMtime_ = 0;
    }

    if (svc_.renderer) {
        svc_.renderer->setScene(svc_.scene);
        svc_.renderer->refreshFrameData();
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::SceneOpened, 0});
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    spdlog::info("[io] 场景已载入: {} ({} 结构 / {} 体系 / {} 实例)", path.string(),
                 snap.structures.size(), snap.systems.size(), snap.instances.size());
    return true;
}

bool IoController::exportPng(const std::filesystem::path& path, int scale) {
    (void)path;
    (void)scale;
    spdlog::warn("[io] exportPng: M4 落地（离屏渲染 + PNG 编码）");
    return false;
}

void IoController::onEvent(const Event&) {
    // 当前无 IoController 关心的事件（原 ScriptRequest 通道随 ScriptController 移除）
}

} // namespace moleculens::app
