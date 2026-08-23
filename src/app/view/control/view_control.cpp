// =============================================================================
// MolecuLens · app · view/control/view_control.cpp —— 顶层视图控制器实现
// 规格来源：主方案 §7.2/§7.3/§7.4/§7.5（主循环；自绘标题栏；Dock 布局
//           WorkSize 而非 DisplaySize；加载遮罩状态机）
// =============================================================================

#include "app/view/control/view_control.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#include <commdlg.h>
#define GLFW_EXPOSE_NATIVE_WIN32   // glfwGetWin32Window（自绘标题栏手动最大化）
#include <GLFW/glfw3native.h>
#endif

#include "app/controller/io/io_controller.h"
#include "app/controller/selection/selection_controller.h"
#include "app/controller/timeline/timeline_controller.h"
#include "app/data/data_manager.h"
#include "app/shell/application.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/timeline/time_axis.h"
#include "app/view/tools/export/export_img_tool.h"
#include "app/view/tools/material/material_tool.h"
#include "app/view/tools/mol_list/mol_list_tool.h"
#include "app/view/tools/paint/painting_tool.h"
#include "app/view/tools/styles/styles_tool.h"
#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "persist/settings/settings.h"

#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

// 文件打开对话框（Windows；PDB/GRO/MLSCENE 过滤器，主方案 §7.3 打开 Ctrl+O）
std::string openFileDialog() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter =
        "分子结构文件 (*.pdb;*.gro)\0*.pdb;*.gro\0"
        "场景文件 (*.mlscene)\0*.mlscene\0"
        "所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        return path;
    }
#else
    // 非 Windows：占位（M5 接入 native dialog）
#endif
    return {};
}

// 上次导出目录（会话内记忆；首次为空 → 由系统决定/当前目录）。
// "导出图像"对话框下次默认定位到该目录。
std::string& lastExportDir() {
    static std::string dir;
    return dir;
}

// 自绘标题栏窗口按钮图标（绘制列表画，不依赖字体字形——最大化/还原需随状态切换）
enum class WindowGlyph { Minimize, Maximize, Restore, Close };

void drawWindowGlyph(const ImVec2& pos, const ImVec2& size, WindowGlyph glyph) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = IM_COL32(208, 211, 222, 255);
    const float t = 2.0f;
    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    switch (glyph) {
        case WindowGlyph::Minimize:
            dl->AddLine(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x + 6.0f, c.y), col, t);
            break;
        case WindowGlyph::Maximize:
            dl->AddRect(ImVec2(c.x - 6.0f, c.y - 5.0f), ImVec2(c.x + 6.0f, c.y + 5.0f),
                        col, 0.0f, 0, t);
            break;
        case WindowGlyph::Restore: {
            // 两个错位方框（后框 + 前景底 + 前框），表示"还原"
            const ImVec2 a(c.x - 3.0f, c.y - 4.0f), b(c.x + 6.0f, c.y + 5.0f);
            const ImVec2 a2(c.x - 6.0f, c.y - 5.0f), b2(c.x + 3.0f, c.y + 4.0f);
            dl->AddRect(a2, b2, col, 0.0f, 0, t);
            dl->AddRectFilled(a, b, IM_COL32(26, 30, 45, 255));
            dl->AddRect(a, b, col, 0.0f, 0, t);
            break;
        }
        case WindowGlyph::Close:
            dl->AddLine(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f), col, t);
            dl->AddLine(ImVec2(c.x - 5.0f, c.y + 5.0f), ImVec2(c.x + 5.0f, c.y - 5.0f), col, t);
            break;
    }
}

} // namespace

// 平台原生"另存为"对话框（Windows 资源管理器；PNG 过滤器）。
// 用户确认返回所选路径；取消返回空串。成功后记忆目录供下次默认定位。
std::string ViewControl::saveFileDialog(const char* defaultName) {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    // 默认文件名（无扩展名自动补 .png）
    std::string name = (defaultName && *defaultName) ? defaultName : "image.png";
    if (name.find('.') == std::string::npos) {
        name += ".png";
    }
    std::snprintf(path, sizeof(path), "%s", name.c_str());

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "PNG 图像 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "png";   // 用户未输扩展名时自动补
    // OFN_NOCHANGEDIR：对话框不改应用当前工作目录（避免相对路径副作用）；
    // OFN_OVERWRITEPROMPT：覆盖前询问；OFN_PATHMUSTEXIST：路径必须存在。
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const std::string& initDir = lastExportDir();
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();

    if (GetSaveFileNameA(&ofn)) {
        const std::string chosen(path);
        // 记忆目录（供下次默认定位到同一文件夹）
        const size_t slash = chosen.find_last_of("\\/");
        lastExportDir() =
            (slash == std::string::npos) ? std::string() : chosen.substr(0, slash);
        return chosen;
    }
#else
    (void)defaultName;   // 非 Windows：占位（M5 接入 native dialog）
#endif
    return {};
}

ViewControl::~ViewControl() { shutdown(); }

bool ViewControl::init(engine::VulkanRenderer* vk, engine::Renderer* renderer,
                       engine::OrbitCamera* camera, AppServices& svc) {
    vk_ = vk;
    renderer_ = renderer;
    camera_ = camera;
    svc_ = &svc;
    dataMgr_ = svc.data;
    timelineCtl_ = nullptr;   // 由 Application 装配后注入
    ioCtl_ = nullptr;

    spdlog::info("[ui] init: creating GLFW window ...");
    if (!createWindow()) {
        spdlog::error("[ui] init: createWindow FAILED");
        return false;
    }
    spdlog::info("[ui] init: GLFW window created");
    // Vulkan 渲染器（主方案 §6.3 初始化流程：实例/设备/交换链/管线）
    // 注：Vulkan 校验层（cfg.debug=true）在本机 loader 加载时崩溃（环境问题：
    // 搜狗输入法钩子与 loader 交互）。默认关闭以保证稳定运行；
    // 需要校验层时置 true 并在干净环境运行（M0 验收项在干净环境达成）。
    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 4;
    std::string vkError;
    spdlog::info("[ui] init: VulkanRenderer init (instance/device/swapchain) ...");
    if (!vk_->init(window_, cfg, &vkError)) {
        spdlog::error("[ui] VulkanRenderer init failed: {}", vkError);
        return false;
    }
    spdlog::info("[ui] init: VulkanRenderer ok");
    if (!initImGuiBackends()) {
        spdlog::error("[ui] init: ImGui backends init FAILED");
        return false;
    }
    spdlog::info("[ui] init: ImGui backends ok");
    // 设计系统（主方案 §7.6）
    UITheme::applyTheme();
    spdlog::info("[ui] init: theme applied");

    // 设置加载（主方案 §12.2：布局 base64 恢复 + 相机/渲染参数）
    spdlog::info("[ui] init: loading settings ...");
    auto settingsResult = persist::SettingsStore::load();
    if (settingsResult) {
        const auto& settings = settingsResult.value();
        if (!settings.layout.empty()) {
            deserializeLayout(settings.layout);
        }
        if (settings.recentFiles.size() > 0) {
            // M5：最近文件菜单（当前仅记录）
            (void)settings;
        }
    }
    // 面板注册（主方案 §7.9：5 个工具面板 + 日志）
    {
        auto ml = std::make_unique<MolListTool>();
        molListTool_ = ml.get();
        addModule(std::move(ml));
    }
    {
        auto st = std::make_unique<StylesTool>();
        stylesTool_ = st.get();
        addModule(std::move(st));
    }
    {
        auto mt = std::make_unique<MaterialTool>();
        materialTool_ = mt.get();
        addModule(std::move(mt));
    }
    addModule(std::make_unique<ExportImgTool>());
    addModule(std::make_unique<PaintingTool>());
    spdlog::info("[ui] init: panels registered");
    spdlog::info("[ui] init: MolScene viewport (offscreen renderer) ...");
    molScene_.init(renderer_, camera_, vk_);
    spdlog::info("[ui] init: MolScene viewport ready");
    return true;
}

void ViewControl::shutdown() {
    // ImGui 后端与窗口清理（Vulkan 设备由 VulkanRenderer 管理）
    if (window_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
}

bool ViewControl::createWindow() {
    if (!glfwInit()) {
        spdlog::error("[ui] glfwInit failed");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);       // Vulkan 专用
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);         // 自绘标题栏（主方案 §7.3）
    window_ = glfwCreateWindow(1280, 800, "MolecuLens", nullptr, nullptr);
    if (!window_) {
        spdlog::error("[ui] glfwCreateWindow failed");
        return false;
    }
    return true;
}

bool ViewControl::initImGuiBackends() {
    // ImGui 上下文
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;    // Dock（docking-experimental）
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 字体必须先于 Vulkan 后端初始化（CreateFontsTexture 使用 io.Fonts 当前字体，
    // 先加载字体再建纹理，主方案 §7.6）
    UITheme::loadFont("font/OPPO_B.ttf");
    // GLFW 后端
    if (!ImGui_ImplGlfw_InitForVulkan(window_, true)) {
        return false;
    }
    // Vulkan 后端（使用引擎设备/队列/交换链）
    engine::VulkanDevice& device = vk_->device();
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = device.instance();
    info.PhysicalDevice = device.physicalDevice();
    info.Device = device.device();
    info.QueueFamily = device.graphicsFamily();
    info.Queue = device.graphicsQueue();
    info.DescriptorPool = vk_->imGuiPool();
    info.RenderPass = vk_->mainRenderPass();
    info.MinImageCount = 3;
    info.ImageCount = static_cast<uint32_t>(device.swapchainImages().size());
    info.MSAASamples = vk_->msaaSamples();
    info.UseDynamicRendering = false;
    info.ApiVersion = VK_API_VERSION_1_2;

    // 项目全局定义 VK_NO_PROTOTYPES（volk 动态加载）→ imgui_impl_vulkan.cpp
    // 编译为无原型模式，Init 前必须先 LoadFunctions（否则 IM_ASSERT 失败 → abort）
    if (!ImGui_ImplVulkan_LoadFunctions(
            info.ApiVersion,
            [](const char* function_name, void* user_data) {
                return vkGetInstanceProcAddr(
                    reinterpret_cast<VkInstance>(user_data), function_name);
            },
            reinterpret_cast<void*>(info.Instance))) {
        return false;
    }
    if (!ImGui_ImplVulkan_Init(&info)) {
        return false;
    }
    // 字体上传（立即执行一次帧命令）
    ImGui_ImplVulkan_CreateFontsTexture();
    return true;
}

void ViewControl::runLoop() {
    // 帧率上限：默认 120 FPS（主方案 §7.2 验收项——帧率不受限导致 CPU/GPU
    // 占用异常；睡眠到帧预算结束，负载低时省资源、帧率稳定在目标附近）。
    constexpr float kTargetFps = 120.0f;
    constexpr double kFrameBudget = 1.0 / static_cast<double>(kTargetFps);
    lastFrameTime_ = static_cast<float>(glfwGetTime());

    /// @brife 进入主循环
    /// frameStart记录本这一帧开始的时刻，后面用来算本帧一共花了多久，做帧率限制
    while (!windowShouldClose_) {
        const double frameStart = glfwGetTime();

        // glfwPollEvents()：处理窗口所有系统事件：鼠标移动、按键、窗口缩放、点击关闭按钮。非阻塞，马上返回。
        glfwPollEvents();
        // glfwWindowShouldClose(window_)：读取窗口是否收到关闭信号（点右上角 ×），赋值给标记，下次循环就退
        windowShouldClose_ = glfwWindowShouldClose(window_);

        const auto now = static_cast<float>(glfwGetTime());
        const float dt = std::min(now - lastFrameTime_, 0.1f);
        lastFrameTime_ = now;
        fps_ = fps_ * 0.95f + (dt > 0.0f ? 1.0f / dt : 0.0f) * 0.05f;
        UITheme::advanceTime(dt);
        if (svc_->bus) {
            svc_->bus->poll();
        }
        // QS.5 文件导入多线程：后台解析完成 → 主线程合并（渲染器读场景须在主线程）
        if (ioCtl_) {
            ioCtl_->consumeParsed();
        }

        newFrame(dt);
        render();
        ImGui::Render();
        // UI 回调注入（renderpass 内 3D 后调用，主方案 §6.3 帧循环）
        vk_->setUiRenderCallback(&ViewControl::renderImGuiUi);
        int fbW = 1280, fbH = 800;
        glfwGetFramebufferSize(window_, &fbW, &fbH);
        engine::RenderView view;
        view.camera = *camera_;
        view.width = static_cast<uint32_t>(std::max(1, fbW));
        view.height = static_cast<uint32_t>(std::max(1, fbH));
        // 帧循环两阶段（主方案 §6.3）：beginFrame 必须先于 execute/endFrame，
        // 否则 frameCommand_ 为空、主交换链不提交（窗口空白且掩盖首帧崩溃）。
        engine::SceneCommandList empty;
        vk_->beginFrame();
        vk_->execute(empty, view);
        vk_->endFrame();

        // 帧率限制：本帧耗时 < 预算 → 睡眠补足剩余时间（稳定上限，省资源）
        const double elapsed = glfwGetTime() - frameStart;
        const double remain = kFrameBudget - elapsed;
        if (remain > 0.0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(remain));
        }
    }
    // 退出时保存设置（主方案 §12.2：布局 base64 + 相机）
    persist::Settings settings;
    auto loaded = persist::SettingsStore::load();
    if (loaded) {
        settings = loaded.value();
    }
    settings.layout = serializeLayout();
    persist::SettingsStore::save(settings);
    spdlog::info("[ui] settings saved on exit");
}

void ViewControl::newFrame(float dt) {
    ImGui_ImplGlfw_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    (void)dt;
}

void ViewControl::render() {
    renderMainMenuBar();
    setupDocking();

    // 工具面板（ToolView 路由器，主方案 §7.1）
    tools_.renderAll(dataMgr_, &molScene_);

    // 主区域容器（3D 视口 + 时间轴 + 状态栏）
    RenderContext ctx;
    ctx.showTimeline = &showTimeline_;
    ctx.showStatusBar = &showStatusBar_;
    ctx.dm = dataMgr_;
    ctx.scene = &molScene_;
    ctx.timeAxis = &timeAxis_;
    ctx.timeline = timelineCtl_;
    ctx.selection = selectionCtl_;
    ctx.fps = fps_;
    mainView_.renderAll(ctx);

    // 日志面板（spdlog 环形缓冲，主方案 §14.5）
    if (showLog_) {
        ImGui::Begin("日志", &showLog_);
        ImGui::TextDisabled("spdlog 分级消息（环形缓冲 2000 条）");
        ImGui::Separator();
        ImGui::TextWrapped("打开文件（Ctrl+O）后在此查看 [io]/[backend] 日志。");
        ImGui::End();
    }

    renderLoadingOverlay();
}


void ViewControl::renderMainMenuBar() {

    // 自绘标题栏（主方案 §7.3：菜单 + 拖拽区 + 居中标题 + 窗口按钮）。
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 34));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::C_SURFACE);
    ImGui::Begin("##titlebar", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoSavedSettings);
    // 记录标题栏实际高度（setupDocking 用它做 DockSpace 偏移，随字号/内边距自适应）
    titleBarHeight_ = ImGui::GetWindowHeight();
    if (ImGui::BeginMenuBar()) {
        // 菜单（主方案 §7.3）
        if (ImGui::BeginMenu("文件")) {
            if (ImGui::MenuItem("打开 (Ctrl+O)")) {
                const std::string path = openFileDialog();
                if (!path.empty() && ioCtl_) {
                    // 加载遮罩（主方案 §7.5）+ 同步加载
                    beginLoading(path, 0);
                    ioCtl_->openFile(path);
                }
            }
            ImGui::MenuItem("导入轨迹", nullptr, false);
            if (ImGui::BeginMenu("导出")) {
                ImGui::MenuItem("图像 (PNG)", nullptr, false);
                ImGui::MenuItem("场景 (OBJ/STL)", nullptr, false);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("退出", "Alt+F4")) {
                windowShouldClose_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("编辑")) {
            ImGui::MenuItem("撤销", "Ctrl+Z", false);
            ImGui::MenuItem("重做", "Ctrl+Y", false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("视图")) {
            ImGui::MenuItem("重置视角", "R", false, false);
            ImGui::Separator();
            ImGui::MenuItem("分子列表", nullptr, &showMolList_);
            ImGui::MenuItem("渲染设置", nullptr, &showStyles_);
            ImGui::MenuItem("材质选择", nullptr, &showMaterial_);
            ImGui::MenuItem("图像导出", nullptr, &showExport_);
            ImGui::MenuItem("绘图工具", nullptr, &showPaint_);
            ImGui::MenuItem("时间轴", nullptr, &showTimeline_);
            ImGui::MenuItem("状态栏", nullptr, &showStatusBar_);
            ImGui::MenuItem("日志", nullptr, &showLog_);
            ImGui::Separator();
            if (ImGui::MenuItem("布局重置")) {
                resetLayout();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("帮助")) {
            ImGui::MenuItem("关于 MolecuLens 0.1.0", nullptr, false);
            ImGui::EndMenu();
        }


        // —— 标题栏右侧控件：窗口按钮 + 居中标题 + 拖拽区 ——
        // 宽度取视口尺寸：自绘标题栏铺满整个窗口，最大化后随之变宽不溢出
        const float barW = viewport->Size.x;
        const float bw = 32.0f, bh = 26.0f;              // 窗口按钮尺寸
        const float buttonsW = 3.0f * bw + 8.0f;         // 三按钮 + 按钮间距
        const ImVec2 row = ImGui::GetCursorScreenPos();  // 菜单之后的行位置

        // 同步真实窗口状态（外部最大化/还原/任务栏操作后 flag 可能过期），
        // 保证按钮图标与切换逻辑与窗口实际状态一致。
        maximized_ = windowIsMaximized();

        // 1) 窗口按钮（最小化/最大化/关闭）—— 靠右，距右缘 8px
        const float btnX = viewport->Pos.x + barW - buttonsW - 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(btnX, row.y));
        if (ImGui::Button("\xe2\x80\x94", ImVec2(bw, bh))) {          // 最小化
            glfwIconifyWindow(window_);
        }
        // 最大化/还原：图标随状态切换（□ 最大化 / 双框 还原），点击切换
        ImGui::SetCursorScreenPos(ImVec2(btnX + bw, row.y));
        ImGui::PushID("maximize");
        if (ImGui::Button("", ImVec2(bw, bh))) {
            toggleMaximize();
        }
        const bool mz = maximized_;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(mz ? "还原" : "最大化");
        }
        drawWindowGlyph(ImGui::GetItemRectMin(), ImGui::GetItemRectSize(),
                        mz ? WindowGlyph::Restore : WindowGlyph::Maximize);
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(btnX + 2 * bw, row.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::C_DANGER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::C_DANGER);
        if (ImGui::Button("×", ImVec2(bw, bh))) {          // 关闭
            windowShouldClose_ = true;
        }
        ImGui::PopStyleColor(3);

        // 2) 居中标题（相对整个标题栏水平居中，垂直居中于按钮高度）
        const ImVec2 titleSize = ImGui::CalcTextSize("MolecuLens");
        ImGui::SetCursorScreenPos(
            ImVec2(viewport->Pos.x + (barW - titleSize.x) * 0.5f,
                   row.y + (bh - titleSize.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::C_ACCENT_HI);
        ImGui::Text("MolecuLens");
        ImGui::PopStyleColor();

        // 3) 拖拽区 —— 最后绘制，铺满"菜单之后 → 按钮之前"并接收点击；
        //    透明按钮盖在标题上不影响显示，标题区域点击同样可拖动窗口。
        //    拖拽用"按下时光标相对窗口的偏移"定位：每帧窗口位置 =
        //    当前窗口位置 + (光标相对位移)，光标坐标用 glfwGetCursorPos
        //    （窗口相对坐标），避免 MouseDelta 累加在快速移动时漂移。
        const float dragW = (btnX - row.x) - 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::InvisibleButton("##drag", ImVec2(std::max(1.0f, dragW), bh));
        ImGui::PopStyleColor(2);
        // 按下帧（item 尚未进入拖动）就记录偏移——IsItemClicked 只在点击帧为真，
        // 若放在"拖动守卫"内会在点击帧被跳过，导致偏移恒为 0、窗口跳变。
        static float s_grabX = 0.0f, s_grabY = 0.0f;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            double cx, cy;
            glfwGetCursorPos(window_, &cx, &cy);
            s_grabX = static_cast<float>(cx);
            s_grabY = static_cast<float>(cy);
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            double cx, cy;
            glfwGetCursorPos(window_, &cx, &cy);
            int wx, wy;
            glfwGetWindowPos(window_, &wx, &wy);
            glfwSetWindowPos(window_, wx + static_cast<int>(cx - s_grabX),
                             wy + static_cast<int>(cy - s_grabY));
        }

        ImGui::EndMenuBar();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

bool ViewControl::windowIsMaximized() const {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window_);
    if (!hwnd) {
        return maximized_;
    }
    // 原生最大化（WS_MAXIMIZE 置位）
    if (::IsZoomed(hwnd)) {
        return true;
    }
    // 自绘最大化：SetWindowPos 铺满工作区（不置 WS_MAXIMIZE 位，IsZoomed 为假），
    // 用"当前矩形 ≈ 所在监视器工作区"判断，外部还原后矩形变化 → 状态自动同步。
    RECT cur, work{};
    if (!GetWindowRect(hwnd, &cur)) {
        return maximized_;
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (mon && GetMonitorInfo(mon, &mi)) {
        work = mi.rcWork;
    } else {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    }
    constexpr int kTol = 4;   // 像素容差（任务栏/边框/缩放误差）
    return (std::abs(cur.left - work.left) <= kTol &&
            std::abs(cur.top - work.top) <= kTol &&
            std::abs(cur.right - work.right) <= kTol &&
            std::abs(cur.bottom - work.bottom) <= kTol);
#else
    return maximized_;
#endif
}

void ViewControl::toggleMaximize() {
    if (!window_) {
        return;
    }
#ifdef _WIN32
    // 无边框窗口用 glfwMaximizeWindow/glfwRestoreWindow 时 GLFW 内部尺寸不同步
    // （ImGui DisplaySize 停留在旧值，UI 不铺满、还原失效）。改用 Win32 手动
    // 最大化到工作区 / 还原到记录位置，SetWindowPos → WM_SIZE → GLFW 同步尺寸。
    if (HWND hwnd = glfwGetWin32Window(window_)) {
        if (windowIsMaximized()) {
            // 还原
            if (restoreRectValid_) {
                SetWindowPos(hwnd, nullptr, restoreL_, restoreT_,
                             restoreR_ - restoreL_, restoreB_ - restoreT_,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                ShowWindow(hwnd, SW_RESTORE);
            }
        } else {
            // 最大化：先记录当前矩形，再铺满工作区（排除任务栏）
            RECT cur;
            if (GetWindowRect(hwnd, &cur)) {
                restoreRectValid_ = true;
                restoreL_ = cur.left;
                restoreT_ = cur.top;
                restoreR_ = cur.right;
                restoreB_ = cur.bottom;
            }
            RECT work{};
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)};
            if (mon && GetMonitorInfo(mon, &mi)) {
                work = mi.rcWork;
            } else {
                SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
            }
            SetWindowPos(hwnd, HWND_TOP, work.left, work.top,
                         work.right - work.left, work.bottom - work.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        maximized_ = windowIsMaximized();   // 同步到操作后的真实状态
        return;
    }
#endif
    // 非 Windows 兜底
    if (maximized_) {
        glfwRestoreWindow(window_);
        maximized_ = false;
    } else {
        glfwMaximizeWindow(window_);
        maximized_ = true;
    }
}

void ViewControl::setupDocking() {
    // DockSpace 布局（主方案 §7.4：先右分 25%，再下分 5%，right 再下分 55%；
    // 用 WorkSize 而非 DisplaySize）
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 偏移用实测标题栏高度（renderMainMenuBar 每帧测量）：字号/内边距变化后
    // 高度不再是 34，硬编码会导致 DockSpace 上移/下移、与标题栏重叠或留缝。
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + titleBarHeight_));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->Size.x, viewport->Size.y - titleBarHeight_));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // 窗口内边距清零：DockSpace 才能铺满整个 Dock 窗口，不因默认 14px 内边距
    // 在右/下侧溢出。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::C_BG);
    ImGui::Begin("##dockspace", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGuiID dockspaceId = ImGui::GetID("MolecuLensDock");
    // DockSpace 用实际内容区域尺寸：GetWindowSize 含内边距，若仍用它会令
    // Dock 区域超出内容区（右/下溢出、窗口错位）。
    const ImVec2 workSize = ImGui::GetContentRegionAvail();
    ImGui::DockSpace(dockspaceId, workSize, ImGuiDockNodeFlags_None);

    if (!layoutInitialized_) {
        // 默认布局（主方案 §7.4）
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, workSize);

        ImGuiID right = ImGui::DockBuilderSplitNode(
            dockspaceId, ImGuiDir_Right, 0.25f, nullptr, &dockspaceId);
        ImGuiID bot = ImGui::DockBuilderSplitNode(
            dockspaceId, ImGuiDir_Down, 0.10f, nullptr, &dockspaceId);
        ImGuiID rightBot = ImGui::DockBuilderSplitNode(
            right, ImGuiDir_Down, 0.55f, nullptr, &right);

        // 3D 主视口：中央节点禁用标签栏（NoTabBar）——上方不再显示
        // 窗口标签/功能栏（配合视口窗口 NoTitleBar），"主视口上方工具栏
        // 默认隐藏且不再显示"（主方案 §7.4 主视图沉浸式）。
        ImGui::DockBuilderDockWindow("##3D 视口", dockspaceId);
        if (ImGuiDockNode* viewNode = ImGui::DockBuilderGetNode(dockspaceId)) {
            viewNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
        }
        ImGui::DockBuilderDockWindow("分子列表", right);
        ImGui::DockBuilderDockWindow("渲染设置", rightBot);
        ImGui::DockBuilderDockWindow("材质选择", rightBot);
        ImGui::DockBuilderDockWindow("图像导出", rightBot);
        ImGui::DockBuilderDockWindow("绘图工具", rightBot);
        ImGui::DockBuilderDockWindow("时间轴", bot);
        ImGui::DockBuilderDockWindow("状态栏", bot);
        ImGui::DockBuilderDockWindow("日志", bot);
        ImGui::DockBuilderFinish(dockspaceId);
        layoutInitialized_ = true;
    }
    ImGui::End();
}

void ViewControl::resetLayout() {
    layoutInitialized_ = false;   // 下一帧重建默认布局
}

std::string ViewControl::serializeLayout() {
    // base64(imgui_dock_serialized)，主方案 §12.2
    if (ImGui::GetCurrentContext() == nullptr) {
        return {};
    }
    size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    if (ini == nullptr || size == 0) {
        return {};
    }
    const std::string raw(ini, size);
    static const char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    for (size_t i = 0; i < raw.size(); i += 3) {
        const uint32_t a = static_cast<uint8_t>(raw[i]);
        const uint32_t b = i + 1 < raw.size() ? static_cast<uint8_t>(raw[i + 1]) : 0;
        const uint32_t c = i + 2 < raw.size() ? static_cast<uint8_t>(raw[i + 2]) : 0;
        const uint32_t n = (a << 16) | (b << 8) | c;
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += i + 1 < raw.size() ? kBase64[(n >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? kBase64[n & 63] : '=';
    }
    return out;
}

bool ViewControl::deserializeLayout(const std::string& base64) {
    if (base64.empty() || ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    // base64 解码
    static const std::array<int8_t, 256> kRev = [] {
        std::array<int8_t, 256> table;
        table.fill(-1);
        const char* k =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            table[static_cast<uint8_t>(k[i])] = static_cast<int8_t>(i);
        }
        return table;
    }();
    std::string raw;
    raw.reserve(base64.size() / 4 * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (char ch : base64) {
        if (ch == '=' || ch == '\n' || ch == '\r') {
            continue;
        }
        const int8_t v = kRev[static_cast<uint8_t>(ch)];
        if (v < 0) {
            return false;
        }
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    if (raw.empty()) {
        return false;
    }
    // 恢复布局（下次 ImGui::NewFrame 后生效）
    ImGui::LoadIniSettingsFromMemory(raw.c_str(), raw.size());
    layoutInitialized_ = false;   // 强制按保存布局重建 Dock 节点
    spdlog::info("[ui] layout restored from settings ({} bytes)", raw.size());
    return true;
}

void ViewControl::beginLoading(const std::string& fileName, uint64_t fileSize) {
    loading_.stage = LoadingStage::Queued;
    loading_.fileName = fileName;
    loading_.fileSize = fileSize;
    loading_.progress = -1.0f;
    loading_.delayFrames = 0;
    loadingActive_ = true;
}

void ViewControl::updateLoading(const LoadingState& state) {
    loading_ = state;
    loadingActive_ = state.stage != LoadingStage::Done && state.stage != LoadingStage::Failed;
}

void ViewControl::renderLoadingOverlay() {
    if (!loadingActive_) {
        return;
    }
    // 全屏半透明黑幕（主方案 §7.5：WindowBg alpha 0.55，NoInputs 屏蔽交互）
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##loading_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    // 居中卡片（文件名 + 大小 + 阶段指示 + 进度条）
    const float cardW = 420.0f, cardH = 150.0f;
    ImGui::SetCursorPos(ImVec2((viewport->Size.x - cardW) * 0.5f,
                               (viewport->Size.y - cardH) * 0.5f));
    ImGui::BeginChild("##loading_card", ImVec2(cardW, cardH), true);
    ImGui::TextUnformatted(loading_.fileName.c_str());
    ImGui::SameLine();
    // formatFileSize（主方案 §7.5：人性化 KB/MB）
    char sizeBuf[64];
    const double mb = static_cast<double>(loading_.fileSize) / (1024.0 * 1024.0);
    if (mb >= 1.0) {
        std::snprintf(sizeBuf, sizeof(sizeBuf), "(%.1f MB)", mb);
    } else {
        std::snprintf(sizeBuf, sizeof(sizeBuf), "(%.1f KB)",
                      static_cast<double>(loading_.fileSize) / 1024.0);
    }
    ImGui::TextDisabled("%s", sizeBuf);
    ImGui::Separator();
    static const char* stageNames[] = {"排队中", "读取文件", "分析键", "上传 GPU",
                                       "完成", "失败"};
    ImGui::Text("阶段：%s", stageNames[static_cast<int>(loading_.stage)]);
    ImGui::Dummy(ImVec2(0, 8));
    UITheme::loadingBar(loading_.progress, ImVec2(cardW - 40, 8));
    ImGui::EndChild();
    ImGui::End();
    // 完成/失败倒计时关闭（主方案 §7.5：0.5–1s）
    if (loading_.stage == LoadingStage::Done || loading_.stage == LoadingStage::Failed) {
        if (++loading_.delayFrames > 45) {
            loadingActive_ = false;
        }
    }
}

void ViewControl::svcScene(SceneController* ctl) {
    sceneCtl_ = ctl;
    // 分子列表"删除"直通场景控制器
    if (molListTool_) {
        molListTool_->setSceneController(ctl);
    }
}

void ViewControl::svcRender(RenderController* ctl) {
    renderCtl_ = ctl;
    // 渲染设置面板直通背景/雾效参数
    if (stylesTool_) {
        stylesTool_->setRenderController(ctl);
    }
}

void ViewControl::svcRep(RepresentationController* ctl) {
    repCtl_ = ctl;
    // 渲染设置（样式/着色/缩放）与材质面板直通表示控制器
    if (stylesTool_) {
        stylesTool_->setRepController(ctl);
    }
    if (materialTool_) {
        materialTool_->setRepController(ctl);
    }
}

void ViewControl::renderImGuiUi(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

} // namespace moleculens::app
