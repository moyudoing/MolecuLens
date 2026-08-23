// =============================================================================
// MolecuLens · app · controller/io/io_controller.cpp —— 文件 IO 实现
// 规格来源：主方案 §5.8.1（加载 → 显示 静态全流程 ①②③）
// =============================================================================

#include "app/controller/io/io_controller.h"

#include <algorithm>
#include <string>
#include <unordered_map>

#include "engine/render/renderer.h"
#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

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
        dst.addTrajectoryInstance(structMap[inst.structureIndex], tm,
                                  inst.trajectory, sysId);
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
        svc_.renderer->setScene(svc_.scene);
        svc_.renderer->setRepresentations(repList());
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

std::vector<engine::Representation> IoController::repList() {
    // 表示列表来自 RepresentationController（由 Application 装配后注入）
    return svc_.renderer->representations().empty()
               ? std::vector<engine::Representation>{}
               : svc_.renderer->representations();
}

bool IoController::saveScene(const std::filesystem::path& path) {
    (void)path;
    spdlog::warn("[io] saveScene: M4 落地（.mlscene 序列化）");
    return false;
}

bool IoController::exportPng(const std::filesystem::path& path, int scale) {
    (void)path;
    (void)scale;
    spdlog::warn("[io] exportPng: M4 落地（离屏渲染 + PNG 编码）");
    return false;
}

void IoController::onEvent(const Event& ev) {
    if (ev.type == EventType::ScriptRequest) {
        // M4：脚本请求解析为文件打开
        spdlog::debug("[io] script request: {}", ev.payload);
    }
}

} // namespace moleculens::app
