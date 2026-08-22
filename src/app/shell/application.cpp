// =============================================================================
// MolecuLens · app · shell/application.cpp —— 组合根装配
// 规格来源：主方案 §3.3（P7：控制器拆分，组合根装配；依赖注入不暴露全局变量；
//           Application 持有 8 控制器 + EventBus）
// =============================================================================

#include "app/shell/application.h"

#include <cstdio>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "app/controller/camera/camera_controller.h"
#include "app/controller/io/io_controller.h"
#include "app/controller/render/render_controller.h"
#include "app/controller/rep/representation_controller.h"
#include "app/controller/scene/scene_controller.h"
#include "app/controller/script/script_controller.h"
#include "app/controller/selection/selection_controller.h"
#include "app/controller/timeline/timeline_controller.h"
#include "app/data/data_manager.h"
#include "app/view/control/view_control.h"
#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/scheduler/frame_scheduler.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "entity/scene/scene.h"

namespace moleculens::app {

Application::Application() = default;

Application::~Application() { shutdown(); }

bool Application::init() {
    // 日志（主方案 §14.5：spdlog 异步 + 滚动文件；分类前缀 [core]/[io]/[engine]…）
    try {
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/moleculens.log", 4 * 1024 * 1024, 3);
        auto logger = std::make_shared<spdlog::logger>("moleculens",
                                                       spdlog::sinks_init_list{fileSink});
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
        spdlog::set_pattern("[%H:%M:%S] [%l] %v");
    } catch (const std::exception& e) {
        spdlog::warn("log file init failed: {}", e.what());
    }

    // 引擎层资源
    renderer_ = std::make_unique<engine::Renderer>();
    camera_ = std::make_unique<engine::OrbitCamera>();
    scheduler_ = std::make_unique<engine::FrameScheduler>();

    // 数据门面
    dataMgr_ = std::make_unique<DataManager>();
    dataMgr_->setScene(&scene_);

    // 服务注入
    services_.scene = &scene_;
    services_.renderer = renderer_.get();
    services_.camera = camera_.get();
    services_.scheduler = scheduler_.get();
    services_.bus = &bus_;
    services_.data = dataMgr_.get();

    // 视图（创建 GLFW 窗口 + ImGui 后端 + VulkanRenderer）
    view_ = std::make_unique<ViewControl>();
    if (!view_->init(vkRenderer(), renderer_.get(), camera_.get(), services_)) {
        return false;
    }
    services_.view = view_.get();

    // 控制器装配（主方案 §3.3：8 个领域控制器）
    sceneCtl_ = std::make_unique<SceneController>(services_);
    renderCtl_ = std::make_unique<RenderController>(services_);
    repCtl_ = std::make_unique<RepresentationController>(services_);
    selectionCtl_ = std::make_unique<SelectionController>(services_);
    timelineCtl_ = std::make_unique<TimelineController>(services_);
    cameraCtl_ = std::make_unique<CameraController>(services_);
    ioCtl_ = std::make_unique<IoController>(services_);
    scriptCtl_ = std::make_unique<ScriptController>(services_);

    // 视图注入控制器（时间轴/IO/选择/渲染/表示）
    view_->timeAxis().setController(timelineCtl_.get());
    view_->svcTimeline(timelineCtl_.get());
    view_->svcIo(ioCtl_.get());
    view_->svcSelection(selectionCtl_.get());
    view_->svcRender(renderCtl_.get());
    view_->svcRep(repCtl_.get());

    // 加载回调 → 加载遮罩（主方案 §7.5）
    ioCtl_->setLoadCallback([this](const app::LoadProgress& p) {
        LoadingState state;
        state.stage = static_cast<LoadingStage>(p.stage);
        state.fileName = p.fileName;
        state.progress = p.progress;
        view_->updateLoading(state);
    });

    // 默认表示（主方案 §7.7：加载即显示球棍）
    renderer_->setScene(&scene_);
    renderer_->setRepresentations(repCtl_->representations());
    renderer_->setVulkanRenderer(vkRenderer());
    renderer_->refreshFrameData();

    // 演示数据（M0/M1 验收：启动自动加载水/甲烷夹具，视口显示结构）
    std::filesystem::path demoPdb;
    for (const auto& cand : {std::filesystem::path("data/demo.pdb"),
                             std::filesystem::path("../../data/demo.pdb"),
                             std::filesystem::path("demo.pdb")}) {
        if (std::filesystem::exists(cand)) {
            demoPdb = cand;
            break;
        }
    }
    if (!demoPdb.empty()) {
        view_->beginLoading(demoPdb.filename().string(),
                            std::filesystem::file_size(demoPdb));
        ioCtl_->openFile(demoPdb);
    }
    spdlog::info("[app] MolecuLens application initialized");
    return true;
}

void Application::shutdown() {
    if (view_) {
        view_->shutdown();
        view_.reset();
    }
    // 控制器在 view 之前保持可用；析构顺序反序
    ioCtl_.reset();
    scriptCtl_.reset();
    cameraCtl_.reset();
    timelineCtl_.reset();
    selectionCtl_.reset();
    repCtl_.reset();
    renderCtl_.reset();
    sceneCtl_.reset();
    dataMgr_.reset();
    renderer_.reset();
    camera_.reset();
    scheduler_.reset();
}

engine::VulkanRenderer* Application::vkRenderer() {
    static engine::VulkanRenderer renderer;
    return &renderer;
}

int Application::run(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (!init()) {
        spdlog::critical("[app] initialization failed");
        return 1;
    }
    view_->runLoop();
    fprintf(stderr, "[s] runLoop returned\n");
    shutdown();
    fprintf(stderr, "[s] shutdown returned\n");
    return 0;
}

} // namespace moleculens::app
