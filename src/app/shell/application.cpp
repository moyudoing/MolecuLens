// =============================================================================
// MolecuLens · app · shell/application.cpp —— 组合根装配
// 规格来源：主方案 §3.3（P7：控制器拆分，组合根装配；依赖注入不暴露全局变量；
//           Application 持有 8 控制器 + EventBus）
// =============================================================================

#include "app/shell/application.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")   // SymInitialize/SymFromAddr（崩溃栈符号化）
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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

namespace {

#ifdef _WIN32
// —— 崩溃处理（主方案 §14.5）：未处理异常 → logs/crash.log 栈回溯（符号化）——
// 不依赖 spdlog（崩溃时 spdlog 可能已损坏）：直接原始文件写；同时尝试 spdlog
// critical（若可用）。SetUnhandledExceptionFilter 在 init 起始注册。
LONG WINAPI handleCrash(EXCEPTION_POINTERS* ep) {
    char ts[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const ULONG_PTR addr = reinterpret_cast<ULONG_PTR>(
        ep->ExceptionRecord->ExceptionAddress);

    std::ofstream out("logs/crash.log", std::ios::app | std::ios::out);
    out << "\n=== MolecuLens crash " << ts << " ===\n";
    out << "ExceptionCode = 0x" << std::hex << code << std::dec << "  ("
        << std::dec << static_cast<long>(code) << ")\n";
    out << "ExceptionAddress = 0x" << std::hex << addr << std::dec << "\n";

    // 栈回溯 + 符号名（Debug 构建有 PDB）
    if (SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
        void* frames[64];
        const USHORT n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* info = reinterpret_cast<SYMBOL_INFO*>(buf);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 255;
        for (USHORT i = 0; i < n; ++i) {
            std::ostringstream line;
            line << "    #" << i << " 0x" << std::hex << frames[i] << std::dec;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(),
                            reinterpret_cast<DWORD64>(frames[i]), &disp, info)) {
                line << "  " << info->Name << "+0x" << std::hex << disp << std::dec;
            }
            out << line.str() << "\n";
        }
        SymCleanup(GetCurrentProcess());
    }
    out.close();

    try {
        spdlog::critical("CRASH: code=0x{:08X} at {:p}", code, (void*)addr);
    } catch (...) {
    }
    // EXCEPTION_EXECUTE_HANDLER：处理异常并终止（保持默认退出码，避免二次崩溃）
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif   // _WIN32

} // namespace

Application::Application() = default;

Application::~Application() { shutdown(); }

bool Application::init() {
    // 崩溃处理注册（主方案 §14.5）：未处理异常 → logs/crash.log 栈回溯
#ifdef _WIN32
    SetUnhandledExceptionFilter(&handleCrash);
#endif
    // 日志（主方案 §14.5：spdlog 滚动文件；分类前缀 [core]/[io]/[engine]…）
    // Debug 构建：附加控制台 + stdout 彩色 sink，日志同时输出到终端方便调试。
    // （Win32 GUI 子系统默认无控制台，不 AllocConsole 则 stdout 无处可写）
    bool consoleAttached = false;
#ifdef _WIN32
#ifndef NDEBUG
    // 仅交互会话挂接控制台：服务/隐藏窗口站下 AllocConsole 可能阻塞，跳过
    // （日志仍写滚动文件）。交互会话 Debug 运行时控制台用于观察日志。
    bool interactive = false;
    if (HWINSTA ws = GetProcessWindowStation()) {
        USEROBJECTFLAGS flags{};
        DWORD len = 0;
        if (GetUserObjectInformationW(ws, UOI_FLAGS, &flags, sizeof(flags), &len)) {
            interactive = (flags.dwFlags & WSF_VISIBLE) != 0;
        }
    }
    if (interactive && ::GetConsoleWindow() == nullptr && ::AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        consoleAttached = true;
    }
#endif
#endif
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/moleculens.log", 4 * 1024 * 1024, 3));
#ifndef NDEBUG
        // Debug 构建：仅当确实挂接了控制台才加 stdout sink
        // （stdout 无效时 stdout_color_sink_mt 构造会抛异常）
        if (consoleAttached) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }
#endif
        auto logger = std::make_shared<spdlog::logger>(
            "moleculens", sinks.begin(), sinks.end());
        spdlog::set_default_logger(logger);
        // 每条消息立即落盘（诊断启动挂起/崩溃：最后一条日志即最后到达的步骤）
        logger->flush_on(spdlog::level::info);
#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);   // Debug 显示 debug 级日志
#endif
        spdlog::set_pattern("[%H:%M:%S] [%l] %v");
    } catch (const std::exception& e) {
        spdlog::warn("log file init failed: {}", e.what());
    }
    spdlog::info("[app] init: logger ready");

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
    spdlog::info("[app] init: engine resources + services ok");

    // 视图（创建 GLFW 窗口 + ImGui 后端 + VulkanRenderer）
    spdlog::info("[app] init: creating ViewControl (GLFW+Vulkan+ImGui) ...");
    view_ = std::make_unique<ViewControl>();
    if (!view_->init(vkRenderer(), renderer_.get(), camera_.get(), services_)) {
        spdlog::error("[app] init: ViewControl::init FAILED");
        return false;
    }
    services_.view = view_.get();
    spdlog::info("[app] init: view initialized");

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
    view_->svcScene(sceneCtl_.get());
    view_->svcRender(renderCtl_.get());
    view_->svcRep(repCtl_.get());
    spdlog::info("[app] init: controllers wired");

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

    // 启动不自动加载演示分子：场景初始为空白，由用户 Ctrl+O 打开文件
    //（原 M0 验收自动加载 demo.pdb 已移除，见 moleculens-fixes）。
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
    shutdown();
    return 0;
}

} // namespace moleculens::app
