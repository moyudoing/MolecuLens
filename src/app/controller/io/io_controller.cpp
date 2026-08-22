// =============================================================================
// MolecuLens · app · controller/io/io_controller.cpp —— 文件 IO 实现
// 规格来源：主方案 §5.8.1（加载 → 显示 静态全流程 ①②③）
// =============================================================================

#include "app/controller/io/io_controller.h"

#include "engine/render/renderer.h"
#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

IoController::IoController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
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

    report(1, -1.0f, nullptr);   // ReadingFile
    auto result = entity::ParserRegistry::defaultRegistry().parseFile(
        path, entity::ParseOptions{});
    if (!result) {
        spdlog::error("[io] load failed: {}", result.errorMessage());
        report(5, 0.0f, nullptr);   // Failed
        return false;
    }

    // 装配（registerStructure/addInstance 已在解析器内完成）
    // 绑定渲染器场景
    if (svc_.renderer) {
        svc_.renderer->setScene(svc_.scene);
        svc_.renderer->setRepresentations(repList());
        svc_.renderer->refreshFrameData();
        svc_.scene->setDirty(entity::DIRTY_ALL);
    }
    report(3, 0.8f, nullptr);   // UploadingGPU
    report(4, 1.0f, nullptr);   // Done

    if (svc_.bus) {
        svc_.bus->publish({EventType::SceneOpened, 0});
        svc_.bus->publish({EventType::MoleculeChanged, 0});
    }
    return true;
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
