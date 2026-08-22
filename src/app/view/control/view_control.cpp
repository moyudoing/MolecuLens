// =============================================================================
// MolecuLens · app · view/control/view_control.cpp —— 顶层视图控制器实现
// 规格来源：主方案 §7.2/§7.3/§7.4/§7.5（主循环；自绘标题栏；Dock 布局
//           WorkSize 而非 DisplaySize；加载遮罩状态机）
// =============================================================================

#include "app/view/control/view_control.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#include <commdlg.h>
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

} // namespace

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

    if (!createWindow()) {
        return false;
    }
    // Vulkan 渲染器（主方案 §6.3 初始化流程：实例/设备/交换链/管线）
    // 注：Vulkan 校验层（cfg.debug=true）在本机 loader 加载时崩溃（环境问题：
    // 搜狗输入法钩子与 loader 交互）。默认关闭以保证稳定运行；
    // 需要校验层时置 true 并在干净环境运行（M0 验收项在干净环境达成）。
    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 4;
    std::string vkError;
    if (!vk_->init(window_, cfg, &vkError)) {
        spdlog::error("[ui] VulkanRenderer init failed: {}", vkError);
        return false;
    }
    if (!initImGuiBackends()) {
        return false;
    }
    // 设计系统（主方案 §7.6）
    UITheme::applyTheme();

    // 设置加载（主方案 §12.2：布局 base64 恢复 + 相机/渲染参数）
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
    addModule(std::make_unique<MolListTool>());
    addModule(std::make_unique<StylesTool>());
    addModule(std::make_unique<MaterialTool>());
    addModule(std::make_unique<ExportImgTool>());
    addModule(std::make_unique<PaintingTool>());
    molScene_.init(renderer_, camera_);
    return true;
}

void ViewControl::shutdown() {
    // ImGui 后端与窗口清理（Vulkan 设备由 VulkanRenderer 管理）
    if (window_) {
        fprintf(stderr, "[s] imgui vulkan\n"); ImGui_ImplVulkan_Shutdown();
        fprintf(stderr, "[s] imgui glfw\n"); ImGui_ImplGlfw_Shutdown();
        fprintf(stderr, "[s] ctx\n"); ImGui::DestroyContext();
        fprintf(stderr, "[s] glfwDestroyWindow\n"); glfwDestroyWindow(window_);
        fprintf(stderr, "[s] view done\n");
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
    lastFrameTime_ = static_cast<float>(glfwGetTime());
    while (!windowShouldClose_) {
        glfwPollEvents();
        windowShouldClose_ = glfwWindowShouldClose(window_);

        const float now = static_cast<float>(glfwGetTime());
        const float dt = std::min(now - lastFrameTime_, 0.1f);
        lastFrameTime_ = now;
        fps_ = fps_ * 0.95f + (dt > 0.0f ? 1.0f / dt : 0.0f) * 0.05f;
        UITheme::advanceTime(dt);
        if (svc_->bus) {
            svc_->bus->poll();
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
    // 自绘标题栏（主方案 §7.3：菜单 + 拖拽区 + 居中标题 + 窗口按钮）
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
        ImGui::EndMenuBar();
    }
    // 拖拽区（InvisibleButton 铺满剩余空间；IsMouseDragging → glfwSetWindowPos，§7.3）
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    const float avail = ImGui::GetContentRegionAvail().x - 100.0f;
    ImGui::InvisibleButton("##drag", ImVec2(avail, 24));
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        int x, y;
        glfwGetWindowPos(window_, &x, &y);
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        glfwSetWindowPos(window_, x + static_cast<int>(delta.x),
                         y + static_cast<int>(delta.y));
    }
    // 居中标题（主方案 §7.3）
    const ImVec2 titlePos = ImGui::GetCursorScreenPos();
    const float titleW = ImGui::CalcTextSize("MolecuLens").x;
    ImGui::SetCursorScreenPos(ImVec2(titlePos.x + (avail - titleW) * 0.5f - 40.0f,
                                     titlePos.y));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::C_ACCENT_HI);
    ImGui::Text("MolecuLens");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(titlePos);
    // 窗口按钮（最小化/最大化/关闭，主方案 §7.3）
    const float bw = 32.0f, bh = 26.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - 3 * bw - 8);
    if (ImGui::Button("—", ImVec2(bw, bh))) {
        glfwIconifyWindow(window_);
    }
    ImGui::SameLine();
    if (ImGui::Button("□", ImVec2(bw, bh))) {
        toggleMaximize();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::C_DANGER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::C_DANGER);
    if (ImGui::Button("×", ImVec2(bw, bh))) {
        windowShouldClose_ = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void ViewControl::toggleMaximize() {
    if (!window_) {
        return;
    }
    // 最大化/还原切换（主方案 §7.3：记住位置）
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
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + 34.0f));
    ImGui::SetNextWindowSize(
        ImVec2(viewport->Size.x, viewport->Size.y - 34.0f));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::C_BG);
    ImGui::Begin("##dockspace", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGuiID dockspaceId = ImGui::GetID("MolecuLensDock");
    const ImVec2 workSize = ImGui::GetWindowSize();
    ImGui::DockSpace(dockspaceId, ImVec2(workSize.x, workSize.y),
                     ImGuiDockNodeFlags_None);

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

        ImGui::DockBuilderDockWindow("3D 视口", dockspaceId);
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

void ViewControl::svcRender(RenderController* ctl) {
    // 预留：面板直接驱动渲染参数（M2 接入）
    (void)ctl;
}

void ViewControl::svcRep(RepresentationController* ctl) {
    // 预留：面板直接驱动表示列表（M2 接入）
    (void)ctl;
}

void ViewControl::renderImGuiUi(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

} // namespace moleculens::app
