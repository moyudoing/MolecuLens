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
#include <functional>
#include <thread>

#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#include <commdlg.h>
#include <timeapi.h>   // timeBeginPeriod/timeEndPeriod（限帧睡眠精度）
#define GLFW_EXPOSE_NATIVE_WIN32   // glfwGetWin32Window（自绘标题栏手动最大化）
#include <GLFW/glfw3native.h>
#endif

#include "app/controller/camera/camera_controller.h"
#include "app/controller/io/io_controller.h"
#include "app/controller/selection/selection_controller.h"
#include "app/controller/timeline/timeline_controller.h"
#include "app/data/data_manager.h"
#include "app/data/log_buffer.h"
#include "app/shell/application.h"
#include "app/i18n/language.h"
#include "app/view/dock/dock_layout.h"       // 默认布局构建 + 布局序列化 + 稳定 ID
#include "app/view/overlay/loading_overlay.h"   // 加载遮罩绘制
#include "app/view/theme/ui_theme.h"
#include "app/view/timeline/time_axis.h"

#include "app/view/tools/export/export_img_tool.h"
#include "app/view/tools/material/material_tool.h"
#include "app/view/tools/mol_list/mol_list_tool.h"
#include "app/view/tools/paint/painting_tool.h"
#include "app/view/tools/reps/reps_tool.h"
#include "app/view/tools/styles/styles_tool.h"
#include "app/view/tools/theme/theme_tool.h"

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "persist/settings/settings.h"

#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

#ifdef _WIN32
// UTF-8 → UTF-16（Win32 W 版 API 必需）。
// 为什么必须走 W 版：`GetOpenFileNameA` 用系统 ANSI 代码页解释路径，中文/日文/
// 含 emoji 或其它非 GBK 字符的目录名会被截成 '?'，于是对话框"看不到目标文件"
// 或选中后路径错乱 —— 用户报的正是这个现象。
std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        // 不是合法 UTF-8（历史调用方可能传 ANSI 串）→ 按系统代码页再试一次
        const int m = ::MultiByteToWideChar(CP_ACP, 0, s.data(),
                                            static_cast<int>(s.size()), nullptr, 0);
        if (m <= 0) {
            return {};
        }
        std::wstring w(static_cast<size_t>(m), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()),
                              w.data(), m);
        return w;
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                          static_cast<int>(s.size()), w.data(), n);
    return w;
}

// UTF-16 → UTF-8（把对话框返回的路径交回 std::string 世界）
std::string narrowUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                          nullptr, nullptr);
    return s;
}

// 对话框 owner：不设 owner 时对话框可能被无边框主窗口挡在下面，看起来像
// "点了没反应"。主窗口是当前前台窗口（自绘标题栏 → 点击时必为前台）。
HWND dialogOwner() {
    HWND hwnd = ::GetActiveWindow();
    return hwnd != nullptr ? hwnd : ::GetForegroundWindow();
}

// 32 KiB 路径缓冲：MAX_PATH(260) 在中文长路径 + 深层目录下真的会溢出，
// 而 Win32 对话框的 nMaxFile 允许更大缓冲（与 IFileOpenDialog 的长路径能力对齐）。
constexpr DWORD kDialogPathChars = 32768;
#endif

// 平台原生"打开文件"对话框的公共实现（Windows；过滤器串由各调用方给出）。
// defExt 为 nullptr 表示不自动补扩展名。
std::string nativeOpenFileDialog(const std::string& filter, const char* defExt) {
#ifdef _WIN32
    // 过滤器串是"说明\0通配\0…\0\0"（UTF-8，由 trItems 逐段翻译生成）。
    // std::wstring 可以内含 '\0'，故透传后 LPWSTR 语义与 ANSI 版一致。
    const std::wstring filterW = widenUtf8(filter);
    const std::wstring defExtW = widenUtf8(defExt != nullptr ? defExt : "");
    std::wstring path(kDialogPathChars, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialogOwner();
    ofn.lpstrFilter = filterW.c_str();
    ofn.lpstrFile = path.data();
    ofn.nMaxFile = kDialogPathChars;
    ofn.lpstrDefExt = defExtW.empty() ? nullptr : defExtW.c_str();
    // OFN_EXPLORER：资源管理器样式（默认视图/图标）；OFN_HIDEREADONLY：只读文件可选；
    // nFilterIndex=1：默认落在第一组过滤器（而不是"所有文件"）。
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    ofn.nFilterIndex = 1;
    if (GetOpenFileNameW(&ofn)) {
        return narrowUtf8(std::wstring(path.c_str()));
    }
#else
    // 非 Windows：占位（M5 接入 native dialog）
    (void)filter;
    (void)defExt;
#endif
    return {};
}

// 文件打开对话框（Windows；分子结构 + 场景过滤器，主方案 §7.3 打开 Ctrl+O）
std::string openFileDialog() {
    // ⚠ 跨行隐式拼接的字面量必须**整体**包一次 tr()：写成 tr(a) tr(b) 不是合法
    //   表达式（相邻字符串字面量的隐式拼接只对字面量成立，对函数调用不成立）。
    // ⚠ 含 '\0' 的多段串必须**逐段**翻译（trItems）：tr(const char*) 的键在第一个
    //   NUL 处截断，整串交给 tr() 永远匹配不上（详见 language.h）。
    // ⚠ 扩展名清单与 ParserRegistry 同源（.pdb/.ent/.gro/.sdf/.mol/.cube/.cub），
    //   新增解析器时这里不必再改 —— 见 Workbench::openFileDialog。
    const std::string filter = trItems({
        "分子结构文件 (*.pdb;*.ent;*.gro;*.sdf;*.mol;*.cube;*.cub)",
        "*.pdb;*.ent;*.gro;*.sdf;*.mol;*.cube;*.cub",
        "场景文件 (*.mlscene)", "*.mlscene",
        "所有文件 (*.*)", "*.*"});
    return nativeOpenFileDialog(filter, nullptr);
}

// 轨迹文件打开对话框（.dcd / .xtc / .trr —— 与 IoController::importTrajectory 的分派一致）
std::string openTrajectoryDialog() {
    const std::string filter = trItems({
        "轨迹文件 (*.dcd;*.xtc;*.trr)", "*.dcd;*.xtc;*.trr",
        "DCD (CHARMM/NAMD) (*.dcd)", "*.dcd",
        "XTC (GROMACS) (*.xtc)", "*.xtc",
        "TRR (GROMACS) (*.trr)", "*.trr",
        "所有文件 (*.*)", "*.*"});
    return nativeOpenFileDialog(filter, nullptr);
}

// 改一个设置字段并立即落盘（load → 改 → save）。
// 与 ThemeTool 里的 persistThemePreference 同一取舍：不持有 Application 的设置
// 副本，避免出现两份设置状态（那正是 export_img_tool 曾"界面值与生效参数不一致"
// 的成因）。这里的字段少、写盘频率低，整份读改写最简单也最不容易错。
void persistSettingField(const std::function<void(persist::Settings&)>& mutate) {
    persist::Settings settings;
    auto loaded = persist::SettingsStore::load();
    if (loaded) {
        settings = loaded.value();
    }
    mutate(settings);
    auto saved = persist::SettingsStore::save(settings);
    if (!saved) {
        spdlog::warn("[ui] 设置保存失败: {}", saved.errorMessage());
    }
}

// 上次导出目录（会话内记忆；首次为空 → 由系统决定/当前目录）。
// "导出图像"对话框下次默认定位到该目录。
std::string& lastExportDir() {
    static std::string dir;
    return dir;
}

} // namespace

// 平台原生"另存为"对话框（Windows 资源管理器；PNG 过滤器）。
// 用户确认返回所选路径；取消返回空串。成功后记忆目录供下次默认定位。
std::string ViewControl::saveFileDialog(const char* defaultName) {
    const std::string filter =
        trItems({"PNG 图像 (*.png)", "*.png", "所有文件 (*.*)", "*.*"});
    return saveFileDialogEx(defaultName, filter.c_str(), "png");
}

std::string ViewControl::saveFileDialogEx(const char* defaultName, const char* filter,
                                          const char* defExt) {
#ifdef _WIN32
    // 默认文件名（无扩展名时按 defExt 补齐）
    std::string name = (defaultName && *defaultName) ? defaultName : "untitled";
    if (name.find('.') == std::string::npos) {
        name += ".";
        name += (defExt != nullptr ? defExt : "dat");
    }
    // 与 nativeOpenFileDialog 同一取舍：W 版 + 显式 owner + 32 KiB 缓冲，
    // 否则中文/特殊字符路径下另存为同样会出错（用户可能分不清"打开"和"保存"哪个坏了）。
    const std::wstring filterW = widenUtf8(filter != nullptr ? filter : "");
    const std::wstring defExtW = widenUtf8(defExt != nullptr ? defExt : "");
    std::wstring path = widenUtf8(name);
    path.resize(kDialogPathChars, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialogOwner();
    ofn.lpstrFilter = filterW.c_str();
    ofn.lpstrFile = path.data();
    ofn.nMaxFile = kDialogPathChars;
    ofn.lpstrDefExt = defExtW.empty() ? nullptr : defExtW.c_str();
    // OFN_NOCHANGEDIR：对话框不改应用当前工作目录（避免相对路径副作用）；
    // OFN_OVERWRITEPROMPT：覆盖前询问；OFN_PATHMUSTEXIST：路径必须存在。
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    ofn.nFilterIndex = 1;
    const std::string& initDir = lastExportDir();
    const std::wstring initDirW = widenUtf8(initDir);
    ofn.lpstrInitialDir = initDirW.empty() ? nullptr : initDirW.c_str();

    if (GetSaveFileNameW(&ofn)) {
        const std::string chosen = narrowUtf8(std::wstring(path.c_str()));
        // 记忆目录（供下次默认定位到同一文件夹）
        const size_t slash = chosen.find_last_of("\\/");
        lastExportDir() =
            (slash == std::string::npos) ? std::string() : chosen.substr(0, slash);
        return chosen;
    }
#else
    (void)defaultName;   // 非 Windows：占位（M5 接入 native dialog）
    (void)filter;
    (void)defExt;
#endif
    return {};
}

// 通用"打开文件"对话框：与 saveFileDialogEx 对称，供面板（ESP 参数面板选择
// density / ESP cube 文件）使用。空串 = 用户取消。
std::string ViewControl::openFileDialogEx(const char* filter, const char* defExt) {
    // ⚠ 关键修复：Win32 过滤器串是 "说明\0通配\0…\0\0"，含**内嵌 NUL**。
    //   `std::string(filter)`（从 const char* 构造）会在第一个 NUL 处截断，
    //   交给对话框后就只剩一句说明、没有任何通配符 —— 表现为"文件类型识别不到、
    //   目标文件不显示"（用户报的 bug）。这里按"双 NUL 结束"显式量长度。
    std::string filterStr;
    if (filter != nullptr) {
        // 上限防止调用方传入未按双 NUL 终止的串时越界读
        constexpr size_t kMaxFilterBytes = 4096;
        size_t n = 0;
        while (n + 1 < kMaxFilterBytes && !(filter[n] == '\0' && filter[n + 1] == '\0')) {
            ++n;
        }
        n += 2;   // 含结尾双 NUL
        filterStr.assign(filter, n);
    }
    return nativeOpenFileDialog(filterStr, defExt);
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
    // 垂直同步：从设置读（旧配置缺该键 → 默认 true，与历史行为一致）
    {
        auto boot = persist::SettingsStore::load();
        if (boot) {
            cfg.vsync = boot.value().vsync;
            // MSAA：此前固定 4，设置里的 msaa 从未生效（I-08 排查顺带修复）
            cfg.msaaSamples = boot.value().msaa > 0 ? boot.value().msaa : 1;
            spdlog::info("[ui] vsync = {} (FIFO), msaa = {}",
                         cfg.vsync ? "on" : "off", cfg.msaaSamples);
        }
    }
    // 排查用覆盖：MOLECULENS_MSAA=0/1/2/4/8 强制采样数
    if (const char* msaaEnv = std::getenv("MOLECULENS_MSAA")) {
        const int v = std::atoi(msaaEnv);
        cfg.msaaSamples = v > 0 ? v : 1;
        spdlog::info("[ui] msaa overridden by env = {}", cfg.msaaSamples);
    }
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
    // 设计系统（主方案 §7.6）：先套默认主题，再按下方的用户配置覆盖。
    // 顺序不能反——applyTheme() 读的是 ui::C_* 全局配色，提前套主题会被这里覆盖掉。
    UITheme::applyTheme();
    spdlog::info("[ui] init: theme applied");

    // 设置加载（主方案 §12.2：布局 base64 恢复 + 主题 + 相机/渲染参数）
    spdlog::info("[ui] init: loading settings ...");
    auto settingsResult = persist::SettingsStore::load();
    if (settingsResult) {
        const auto& settings = settingsResult.value();
        // 主题：键未知（旧配置/手改）时 setThemeByKey 返回 false 并保持当前主题，
        // 这里显式记一条日志，避免"配置写了却没生效"变成无声行为。
        if (!settings.theme.empty() && !UITheme::setThemeByKey(settings.theme)) {
            spdlog::warn("[ui] settings.theme='{}' 无法识别，沿用默认主题 {}",
                         settings.theme, UITheme::themeKey());
        }
        // 语言：必须在任何面板构造/渲染之前设置好 —— 面板标题与文案都是 tr()，
        // 晚设会出现"首帧中文、次帧英文"的闪变
        i18n::Language lang = i18n::Language::Zh;
        if (!settings.language.empty() &&
            i18n::languageFromKey(settings.language.c_str(), &lang)) {
            i18n::setLanguage(lang);
        } else if (!settings.language.empty()) {
            spdlog::warn("[ui] settings.language='{}' 无法识别，沿用中文",
                         settings.language);
        }
        if (!settings.layout.empty()) {
            deserializeLayout(settings.layout);
        }
        // M5：最近文件菜单（当前仅记录）
    }

    // 面板注册（主方案 §7.9：5 个工具面板 + 日志）
    // v2：旧工具面板不再构造 —— 它们订阅 EventBus，加载文件时会各自新建/
    // 改写表示（实测启动后表示数从 1 变成 5，并叠出一层半透明大球），
    // 与新工作台的表示管理互相打架。保留代码但不实例化。
#if 0
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
    {
        // 图形表示面板（VMD 式四页签）：表示级增删与全部参数都在这里；
        // styles_tool 只保留场景级设置（背景/雾/光照/视角）
        auto rp = std::make_unique<RepsTool>();
        repsTool_ = rp.get();
        addModule(std::move(rp));
    }
    addModule(std::make_unique<ExportImgTool>());
    addModule(std::make_unique<PaintingTool>());
    addModule(std::make_unique<ThemeTool>());
#endif


    spdlog::info("[ui] init: panels registered");
    spdlog::info("[ui] init: MolScene viewport (offscreen renderer) ...");
    molScene_.init(renderer_, camera_, vk_);
    spdlog::info("[ui] init: MolScene viewport ready");
    // 表面生成异步化（REMAINING_WORK §1.1）：整体系 QuickSurf 在后台线程生成，
    // 主线程不再阻塞约 197 ms；生成期间保留旧画面并在视口左上角提示"表面生成中…"。
    // CLI / 导出 / 单元测试不走此开关（默认同步），行为与既有一致。
    if (renderer_) {
        renderer_->setAsyncSurfaceEnabled(true);
    }

    // v2 Aurora 工作台在首帧延迟装配：控制器由 Application 在 view_->init() 之后
    // 才通过 svcXxx() 注入，这里过早读它们会拿到空指针。
    return true;
}

void ViewControl::initWorkbench() {
    WorkbenchServices ws;
    ws.vk = vk_;
    ws.renderer = renderer_;
    ws.data = dataMgr_;
    ws.viewport = &molScene_;
    ws.scene = svc_ != nullptr ? svc_->scene : nullptr;
    ws.sceneCtl = sceneCtl_;
    ws.renderCtl = renderCtl_;
    ws.repCtl = repCtl_;
    ws.timelineCtl = timelineCtl_;
    ws.cameraCtl = cameraCtl_;
    ws.ioCtl = ioCtl_;
    ws.fps = &fps_;
    ws.requestClose = [this]() { windowShouldClose_ = true; };
    ws.minimizeWindow = [this]() { glfwIconifyWindow(window_); };
    ws.toggleMaximize = [this]() { this->toggleMaximize(); };
    ws.isMaximized = [this]() { return windowIsMaximized(); };
    ws.dragWindow = [this](float dx, float dy) {
        int wx = 0;
        int wy = 0;
        glfwGetWindowPos(window_, &wx, &wy);
        glfwSetWindowPos(window_, wx + static_cast<int>(dx), wy + static_cast<int>(dy));
    };
    ws.dlgOpen = [](const char* filter, const char* defExt) {
        return ViewControl::openFileDialogEx(filter, defExt);
    };
    ws.dlgSave = [](const char* name, const char* filter, const char* defExt) {
        return ViewControl::saveFileDialogEx(name, filter, defExt);
    };
    workbench_.init(ws);
    // 场景标注桥（v2.3）：保存 .mlscene 时把当前标注交给 IoController 写进文件；
    // 打开 .mlscene 时把文件里的标注回填到工作台。控制器不直接依赖表现层。
    if (ioCtl_ != nullptr) {
        ioCtl_->setAnnotationBridge(
            [this]() { return workbench_.exportAnnotations(); },
            [this](const persist::mlscene::Annotations& annotations) {
                workbench_.importAnnotations(annotations);
            });
    }
    spdlog::info("[ui] workbench v2 ready (fixed five-region layout)");
}

void ViewControl::shutdown() {
    // 断开标注桥：回调捕获了 this（Workbench 成员），窗口销毁后不能再被控制器调用
    if (ioCtl_ != nullptr) {
        ioCtl_->setAnnotationBridge(nullptr, nullptr);
    }
    // ImGui 后端与窗口清理（Vulkan 设备由 VulkanRenderer 管理）
    if (window_) {
        // 先释放 MolScene 注册的纹理描述符：它们由 ImGui 描述符池分配，
        // 而池在后端 Shutdown 时销毁 —— 之后释放会操作已销毁的池。
        // （MolScene 的析构发生在 ViewControl 成员销毁阶段，晚于此处。）
        molScene_.releaseImGuiTextures();
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
    // 默认窗口按显示器工作区自适应：1920×1080 上 1280×800 偏小，右检查器与
    // 表示表格会被挤得过密（"UI 看不清"的体感来源之一）。有空间就给 1440×900，
    // 小屏仍回落到 1280×800 / 工作区尺寸。
    int winW = 1440;
    int winH = 900;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor()) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
        if (mw > 0 && mh > 0) {
            winW = std::min(winW, std::max(960, mw - 80));
            winH = std::min(winH, std::max(640, mh - 80));
        }
    }
    spdlog::info("[ui] window size {}x{} (adaptive to monitor work area)", winW, winH);
    window_ = glfwCreateWindow(winW, winH, "MolecuLens", nullptr, nullptr);
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
    UITheme::loadFont("font/NotoSansSC-Medium-subset.ttf");
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
    // 收尾自旋窗口：Windows 默认计时器粒度 ~15.6ms，sleep_for 会把"还剩 8ms"
    // 的帧睡成 15ms+（实测 120FPS 目标被拖到 ~107FPS）。用 timeBeginPeriod(1)
    // 把粒度降到 1ms，且最后 ~1.2ms 改为自旋收尾，保证精确落在帧预算上。
    constexpr double kSpinMarginSec = 0.0012;
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    lastFrameTime_ = static_cast<float>(glfwGetTime());

    /// @brife 进入主循环
    /// frameStart记录本这一帧开始的时刻，后面用来算本帧一共花了多久，做帧率限制
    while (!windowShouldClose_) {

        // 帧开始
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
            // 传入实测 dt：时间轴据此推进播放（与渲染帧率解耦，速度可复现）
            svc_->bus->poll(dt);
        }

        // QS.5 文件导入多线程：后台解析完成 → 主线程合并（渲染器读场景须在主线程）
        if (ioCtl_) {
            ioCtl_->consumeParsed();
            // 命令行给了轨迹文件：等结构解析并合并进场景后再导入（见
            // setPendingTrajectory 的时序说明）
            if (!pendingTrajectory_.empty() && svc_ && svc_->scene &&
                !svc_->scene->instances().empty()) {
                const std::string trajPath = pendingTrajectory_;
                pendingTrajectory_.clear();
                spdlog::info("[ui] 命令行轨迹导入: {}", trajPath);
                if (!ioCtl_->importTrajectory(trajPath)) {
                    spdlog::error("[ui] 命令行轨迹导入失败: {}", trajPath);
                }
            }
        }


        // 帧更新
        newFrame(dt);


        // 渲染
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

        // 帧时间分布（每 5 s 汇总一条日志）。必须在**限帧睡眠之前**取工作量：
        // 周期含睡眠、工作量不含 —— 只看周期会把"限帧在起作用"误判成"性能到顶"
        //（本机实测两者差 3 倍以上，结论完全不同）。
        recordFrameTime(dt * 1000.0f,
                        static_cast<float>((glfwGetTime() - frameStart) * 1000.0));

        // 帧率限制：本帧耗时 < 预算 → 先睡到"预算 - 自旋窗口"，再用自旋精确等
        // 到帧截止时刻（见函数开头的精度说明）。
        const double deadline = frameStart + kFrameBudget;
        const double remain = deadline - glfwGetTime();
        if (remain > 0.0) {
            if (remain > kSpinMarginSec) {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(remain - kSpinMarginSec));
            }
            while (glfwGetTime() < deadline) {
                std::this_thread::yield();
            }
        }
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif

    // 退出时保存设置（主方案 §12.2：布局 base64 + 相机）
    persist::Settings settings;
    auto loaded = persist::SettingsStore::load();
    if (loaded) {
        settings = loaded.value();
    }
    settings.layout = serializeLayout();
    // 主题由 ThemeTool 在切换时即时落盘；这里再写一次当前值，保证"改完主题后
    // 直接退出"与"重启"两条路径读到的是同一份状态（也兜住外部改过配置的情况）。
    settings.theme = UITheme::themeKey();
    settings.language = i18n::languageKey(i18n::current());
    persist::SettingsStore::save(settings);
    spdlog::info("[ui] settings saved on exit (theme={}, lang={})", settings.theme,
                 settings.language);
}

// 帧时间观测：滚动窗口记录 + 周期汇总日志。开销 = 每帧一次 push 与 5 秒一次的
// 排序（≤600 个 float），相对于整帧成本可忽略，故常开（无需编译开关）。
void ViewControl::recordFrameTime(float dtMs, float workMs) {
    if (frameTimes_.size() < kFrameTimeWindow) {
        frameTimes_.push_back(dtMs);
        workTimes_.push_back(workMs);
    } else {
        frameTimes_[frameTimeCursor_] = dtMs;
        workTimes_[frameTimeCursor_] = workMs;
        frameTimeCursor_ = (frameTimeCursor_ + 1) % kFrameTimeWindow;
    }
    const double now = glfwGetTime();
    if (frameStatsLastLog_ <= 0.0) {
        frameStatsLastLog_ = now;   // 首帧只起算，避免把加载期的首帧计入
        return;
    }
    if (now - frameStatsLastLog_ < kFrameStatsPeriodSec) {
        return;
    }
    frameStatsLastLog_ = now;

    std::vector<float> s = frameTimes_;
    std::sort(s.begin(), s.end());
    if (s.empty()) {
        return;
    }
    auto pct = [&s](double p) {
        const size_t i = static_cast<size_t>(p * (s.size() - 1) + 0.5);
        return s[std::min(i, s.size() - 1)];
    };
    double sum = 0.0;
    for (float v : s) {
        sum += v;
    }
    const double avg = sum / static_cast<double>(s.size());
    // 工作量均值（不含限帧睡眠）与周期对比 —— 见头文件里两者的区别
    double wsum = 0.0;
    float wmax = 0.0f;
    for (float v : workTimes_) {
        wsum += v;
        wmax = std::max(wmax, v);
    }
    const double wavg = workTimes_.empty() ? 0.0
                                           : wsum / static_cast<double>(workTimes_.size());
    // 注意 avg 是"帧时间均值"，最低 FPS 应取**最慢帧**（max 帧时间）的倒数 ——
    // 用平均 FPS 描述流畅度会掩盖卡顿
    // 日志保持中文（诊断输出，不随界面语言切换）；跨行拼接的格式串不能被逐段包 tr()
    spdlog::info("[perf] 整帧 {} samples: avg {:.2f} ms ({:.0f} FPS) | p50 {:.2f} "
                 "p95 {:.2f} p99 {:.2f} max {:.2f} ms (最低 {:.0f} FPS) | 工作量 avg "
                 "{:.2f} max {:.2f} ms (占周期 {:.0f}%)",
                 s.size(), avg, avg > 0.0 ? 1000.0 / avg : 0.0, pct(0.50), pct(0.95),
                 pct(0.99), s.back(), s.back() > 0.0 ? 1000.0 / s.back() : 0.0, wavg,
                 wmax, avg > 0.0 ? 100.0 * wavg / avg : 0.0);
}

void ViewControl::newFrame(float dt) {
    ImGui_ImplGlfw_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    (void)dt;
}

void ViewControl::render() {
    // v2：Aurora 工作台接管全部布局（固定五区 + 9 个检查器模块）。
    // 旧 Dock / 工具面板代码保留在下方 #if 0 中仅供对照，不再进入渲染路径。
    if (!workbenchReady_) {
        workbenchReady_ = true;
        initWorkbench();
    }
    workbench_.render();
    renderLoadingOverlay();
#if 0
    renderMainMenuBar();
    setupDocking();

    // 工具面板（ToolView 路由器，主方案 §7.1）
    tools_.renderAll(dataMgr_, &molScene_);  // 渲染

    /// 主区域容器（3D 视口 + 时间轴 + 状态栏）
    // 构建参数
    RenderContext ctx;
    ctx.showTimeline = &showTimeline_;
    ctx.showStatusBar = &showStatusBar_;
    ctx.dm = dataMgr_;
    ctx.scene = &molScene_;
    ctx.timeAxis = &timeAxis_;
    ctx.timeline = timelineCtl_;
    ctx.selection = selectionCtl_;
    ctx.fps = fps_;
    // 时间轴显示帧率（此前 TimeAxis::showFps 声明后无人写入 → 恒为 0）
    timeAxis_.setFps(fps_);
    // 主视图渲染
    moleculens::app::MainView::renderAll(ctx);

    // 日志面板（spdlog 环形缓冲，主方案 §14.5）
    if (showLog_) {
        // 显示名可翻译、ID 恒定（同 dock_layout.h 的说明；见 main_view 的用法）
        const std::string logLabel =
            dock::windowLabel(i18n::tr("日志"), dock::panel::kLog);
        ImGui::Begin(logLabel.c_str(), &showLog_);

        auto& ring = LogRingBuffer::instance();
        if (ImGui::SmallButton(tr("清空"))) {
            ring.clear();
            logSnapshot_.clear();
            logSeen_ = ring.totalPushed();
        }
        ImGui::SameLine();
        ImGui::Checkbox(tr("自动滚动"), &logAutoScroll_);
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%zu 条 / 上限 %zu"), logSnapshot_.size(),
                            LogRingBuffer::kCapacity);
        ImGui::Separator();

        // 有新日志才重取快照（缓冲满后 size() 恒定，靠累计计数判断）
        if (ring.totalPushed() != logSeen_) {
            logSnapshot_ = ring.snapshot();
            logSeen_ = ring.totalPushed();
        }

        ImGui::BeginChild("##log_scroll", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& rec : logSnapshot_) {
            // 日志级别配色走主题（此前写死 5 个 ImVec4 → 切主题时日志面板不跟随）
            ImVec4 col = ui::C_TEXT;                 // 默认：正文
            switch (rec.level) {
                case spdlog::level::trace:
                case spdlog::level::debug:    col = ui::C_TEXT_MUTED; break;
                case spdlog::level::warn:     col = ui::C_WARN; break;
                case spdlog::level::err:
                case spdlog::level::critical: col = ui::C_DANGER; break;
                default: break;
            }
            // spdlog 的 to_string_view 返回 fmt::string_view（非 std::string_view
            // 也不保证 NUL 结尾），先格式化到定长缓冲再交给 ImGui
            char lvl[16] = {};
            const auto sv = spdlog::level::to_string_view(rec.level);
            std::snprintf(lvl, sizeof(lvl), "%-8.*s",
                          static_cast<int>(sv.size()), sv.data());
            // 时间 + 级别用 fontSmall()（13px 档）：与正文拉开层级，日志更易扫读。
            // （该字体同样属于"加载了却没用"的一档，此处接上。）
            ImGui::PushFont(UITheme::fontSmall());
            ImGui::TextColored(col, "%s", rec.time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(col, "[%s]", lvl);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", rec.text.c_str());
        }
        // 自动滚动：仅在用户没有上翻（已在底部）时跟随
        if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::End();
    }

    renderLoadingOverlay();
#endif
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
        if (ImGui::BeginMenu(tr("文件"))) {
            if (ImGui::MenuItem(tr("打开 (Ctrl+O)"))) {
                const std::string path = openFileDialog();
                if (!path.empty() && ioCtl_) {
                    // 加载遮罩（主方案 §7.5）+ 同步加载
                    beginLoading(path, 0);
                    ioCtl_->openFile(path);
                }
            }
            if (ImGui::MenuItem(tr("保存场景 (Ctrl+S)"))) {
                // ⚠ 必须是具名的 std::string（不能用 static const char* 接 .c_str()）：
                //   trItems 返回临时串，表达式结束即销毁 → 存指针就是悬垂。
                const std::string sceneFilter =
                    trItems({"MolecuLens 场景 (*.mlscene)", "*.mlscene",
                             "所有文件 (*.*)", "*.*"});
                const std::string path =
                    saveFileDialogEx("scene.mlscene", sceneFilter.c_str(), "mlscene");
                if (!path.empty() && ioCtl_) {
                    ioCtl_->saveScene(path);
                }
            }
            if (ImGui::MenuItem(tr("导入轨迹 (.dcd/.xtc)"))) {
                const std::string path = openTrajectoryDialog();
                if (!path.empty() && ioCtl_) {
                    ioCtl_->importTrajectory(path);
                }
            }
            if (ImGui::BeginMenu(tr("导出"))) {
                // 图像导出面板已可用（ExportImgTool）；此处保留入口占位
                ImGui::MenuItem(tr("图像 (PNG)"), nullptr, false);
                ImGui::MenuItem(tr("场景 (OBJ/STL)"), nullptr, false);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(tr("退出"), "Alt+F4")) {
                windowShouldClose_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("编辑"))) {
            ImGui::MenuItem(tr("撤销"), "Ctrl+Z", false);
            ImGui::MenuItem(tr("重做"), "Ctrl+Y", false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("视图"))) {
            if (ImGui::MenuItem(tr("重置视角"), "R")) {
                if (cameraCtl_) {
                    cameraCtl_->reset();   // 经相机控制器（唯一入口）
                }
            }
            ImGui::Separator();
            ImGui::MenuItem(tr("分子列表"), nullptr, &showMolList_);
            ImGui::MenuItem(tr("渲染设置"), nullptr, &showStyles_);
            ImGui::MenuItem(tr("材质选择"), nullptr, &showMaterial_);
            ImGui::MenuItem(tr("图像导出"), nullptr, &showExport_);
            ImGui::MenuItem(tr("绘图工具"), nullptr, &showPaint_);
            ImGui::MenuItem(tr("时间轴"), nullptr, &showTimeline_);
            ImGui::MenuItem(tr("状态栏"), nullptr, &showStatusBar_);
            ImGui::MenuItem(tr("日志"), nullptr, &showLog_);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("布局重置"))) {
                resetLayout();
            }
            ImGui::EndMenu();
        }
        // —— 设置：语言 / 主题（两大外观开关都收敛在这里，便于发现）——
        if (ImGui::BeginMenu(tr("设置"))) {
            if (ImGui::BeginMenu(tr("语言"))) {
                for (int i = 0; i < static_cast<int>(i18n::Language::Count); ++i) {
                    const auto lang = static_cast<i18n::Language>(i);
                    // 语言名用**各自语言**书写：切错语言后用户仍认得出自己的那一项
                    if (ImGui::MenuItem(i18n::languageNativeName(lang), nullptr,
                                        i18n::current() == lang)) {
                        i18n::setLanguage(lang);
                        persistSettingField([lang](persist::Settings& s) {
                            s.language = i18n::languageKey(lang);
                        });
                    }
                }
                ImGui::EndMenu();
            }
            // 白天/暗黑一键切换（在各自模式最近用过的主题之间切换）。
            // 菜单项文字随当前模式变化：现在是暗色就显示"白天模式"（点了变亮）。
            const bool dark = UITheme::isDarkTheme();
            if (ImGui::MenuItem(tr(dark ? "白天模式" : "暗黑模式"))) {
                UITheme::toggleDarkLight();
                persistSettingField([](persist::Settings& s) {
                    s.theme = UITheme::themeKey();
                });
            }
            UITheme::helpMarker("在「白天」与「暗黑」两套主题间切换；\n"
                                "各自记住你最近选用的那一套（不是固定的一对）。");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("帮助"))) {
            ImGui::MenuItem(tr("关于 MolecuLens V1.0"), nullptr, false);
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
            ImGui::SetTooltip(mz ? tr("还原") : tr("最大化"));
        }
        chrome::drawWindowGlyph(ImGui::GetItemRectMin(), ImGui::GetItemRectSize(),
                                mz ? chrome::WindowGlyph::Restore
                                   : chrome::WindowGlyph::Maximize);
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
        //    用 fontTitle()（20px 档）：此前该字体**加载了却从未使用**，
        //    标题栏与正文同号，白白多加载一档字形。
        ImGui::PushFont(UITheme::fontTitle());
        const ImVec2 titleSize = ImGui::CalcTextSize("MolecuLens");
        ImGui::SetCursorScreenPos(
            ImVec2(viewport->Pos.x + (barW - titleSize.x) * 0.5f,
                   row.y + (bh - titleSize.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::C_ACCENT_HI);
        ImGui::Text("MolecuLens");
        ImGui::PopStyleColor();
        ImGui::PopFont();

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

// 窗口最大化/还原状态机与图标绘制在 app/view/chrome/window_chrome.{h,cpp}；
// 此处只做状态转发（MaximizeState 由本类持有，生命周期与其一致）。
bool ViewControl::windowIsMaximized() const {
    return chrome::windowIsMaximized(window_, maximizeState_);
}

void ViewControl::toggleMaximize() {
    if (!window_) {
        return;
    }
    chrome::toggleMaximize(window_, maximizeState_);
    maximized_ = maximizeState_.maximized;
}

void ViewControl::setupDocking() {
    // DockSpace 布局（主方案 §7.4）。默认布局树的构建与序列化在
    // app/view/dock/dock_layout.{h,cpp}（本函数只负责 dockspace 窗口本身）。
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
        dock::buildDefaultLayout(dockspaceId, workSize);
        layoutInitialized_ = true;
    }
    ImGui::End();
}

void ViewControl::resetLayout() {
    layoutInitialized_ = false;   // 下一帧重建默认布局
}

// 布局序列化/反序列化：实现在 app/view/dock/dock_layout.cpp（与默认布局同一模块）。
// 此处保留同名静态包装以维持既有调用点（Application 启动/退出），
// 并在恢复成功后把 layoutInitialized_ 置 false → 按保存布局重建 Dock 节点。
std::string ViewControl::serializeLayout() { return dock::serializeLayout(); }

bool ViewControl::deserializeLayout(const std::string& base64) {
    if (!dock::deserializeLayout(base64)) {
        return false;
    }
    layoutInitialized_ = false;
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

// 遮罩绘制在 app/view/overlay/loading_overlay.cpp；此处只做状态转发（倒计时
// 由该模块自行处理并回置 loadingActive_）。
void ViewControl::renderLoadingOverlay() {
    overlay::render(loading_, loadingActive_);
}

void ViewControl::svcTimeline(TimelineController* ctl) {
    timelineCtl_ = ctl;
    // 图形表示面板的 Trajectory 页签直接操作时间轴（帧范围/步长是**全局**的，
    // 不是逐表示参数）—— 与时间轴面板共用同一份状态
    if (repsTool_) {
        repsTool_->setTimelineController(ctl);
    }
}

void ViewControl::svcIo(IoController* ctl) {
    ioCtl_ = ctl;
    // 图形表示面板的 ESP 参数需要两样 IO 能力：
    //   · "当前加载文件路径" → 密度 cube 的默认值（打开 density.cube 时就是它）
    //   · 文件打开对话框 → 选择 density / ESP cube
    if (repsTool_) {
        repsTool_->setIoController(ctl);
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
    // 表示控制器只注入**表示级**面板：材质面板与「图形表示」面板。
    // styles_tool 是场景级的（视角/背景/雾/光照），不再持有表示控制器 ——
    // 表示的一切入口收敛到「图形表示」一处。
    if (materialTool_) {
        materialTool_->setRepController(ctl);
    }
    if (repsTool_) {
        repsTool_->setRepController(ctl);
    }
}

void ViewControl::svcCamera(CameraController* ctl) {
    cameraCtl_ = ctl;
    // 视口交互补注入（ViewControl::init 早于控制器装配，构造期拿不到控制器）
    molScene_.setCameraController(ctl);
    // 相机面板（正交/透视 + FOV/复位）直通相机控制器 —— 与视口交互同一入口
    if (stylesTool_) {
        stylesTool_->setCameraController(ctl);
    }
}

void ViewControl::renderImGuiUi(VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

} // namespace moleculens::app
