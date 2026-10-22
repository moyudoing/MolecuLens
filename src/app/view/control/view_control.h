// =============================================================================
// MolecuLens · app · view/control/view_control.h —— 顶层视图控制器
// 规格来源：主方案 §7.1/§7.2/§7.3/§7.4/§7.5（窗口生命周期、ImGui 上下文、
//           主循环、Dock 布局、面板可见性开关、自绘标题栏、加载遮罩状态机）
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder 系列
#include <volk.h>             // VkCommandBuffer（renderImGuiUi 静态回调签名）

#include "app/data/log_buffer.h"   // LogRecord（日志面板快照成员，完整类型）
#include "app/view/chrome/window_chrome.h"      // MaximizeState（成员，完整类型）
#include "app/view/container/main_view.h"
#include "app/view/container/multi_view.h"
#include "app/view/overlay/loading_overlay.h"   // LoadingState（成员，完整类型）
#include "app/view/timeline/time_axis.h"        // TimeAxis 值成员（完整类型）
#include "app/view/tool/tool_registry.h"
#include "app/view/viewport/mol_scene.h"
#include "app/workbench/workbench.h"

namespace moleculens::engine {
class Renderer;
class VulkanRenderer;
class OrbitCamera;
class SceneCommandList;
} // namespace moleculens::engine

namespace moleculens::app {

struct AppServices;   // 定义于 shell/application.h（init 参数引用）

class Application;
class DataManager;
class MolListTool;
class StylesTool;
class MaterialTool;
class RepsTool;
class ExportImgTool;
class PaintingTool;
class TimeAxis;
class TimelineController;
class IoController;
class SelectionController;
class SceneController;
class RenderController;
class RepresentationController;
class CameraController;

// 加载遮罩状态机已抽到 app/view/overlay/loading_overlay.h（本文件 include 之）；
// 保留别名以免既有调用点（Application/IoController）改动。
using LoadingStage = overlay::LoadingStage;
using LoadingState = overlay::LoadingState;

class ViewControl {
public:
    ~ViewControl();

    // 初始化窗口 + ImGui 后端（services 提供 engine 资源）
    bool init(engine::VulkanRenderer* vk, engine::Renderer* renderer,
              engine::OrbitCamera* camera, AppServices& svc);
    void shutdown();

    // 主循环（主方案 §7.2）
    void runLoop();

    // 一行注册新面板（主方案 §7.1）
    void addModule(std::unique_ptr<BaseToolView> tool) { tools_.addModule(std::move(tool)); }

    // 布局重置（主方案 §7.4 验收项）
    void resetLayout();

    // 布局序列化（M5 落地 settings.json）
    static std::string serializeLayout();
    bool deserializeLayout(const std::string& base64);

    // 加载遮罩（主方案 §7.5）
    void beginLoading(const std::string& fileName, uint64_t fileSize);
    void updateLoading(const LoadingState& state);

    // 窗口控制（自绘标题栏，主方案 §7.3）
    void toggleMaximize();

    // 启动时待导入的轨迹（命令行给出"结构文件 + 轨迹文件"时用）。
    // 必须延迟：结构解析是异步的，立即导入会因为没有实例可挂载而失败。
    void setPendingTrajectory(std::string path) { pendingTrajectory_ = std::move(path); }

    [[nodiscard]] GLFWwindow* window() const { return window_; }
    [[nodiscard]] bool windowShouldClose() const { return windowShouldClose_; }
    MolScene& molScene() { return molScene_; }
    TimeAxis& timeAxis() { return timeAxis_; }
    ToolRegistry& tools() { return tools_; }

    // 控制器服务注入（Application 装配）
    void svcTimeline(TimelineController* ctl);   // 定义在 cpp（需 RepsTool 完整类型）
    void svcIo(IoController* ctl);               // 定义在 cpp（IoController 注入 RepsTool）
    void svcSelection(SelectionController* ctl) { selectionCtl_ = ctl; }
    void svcScene(SceneController* ctl);
    void svcRender(RenderController* ctl);
    void svcRep(RepresentationController* ctl);
    // 相机控制器：视口交互与相机面板的共同入口（须在 init 之前注入）
    void svcCamera(CameraController* ctl);

    // UI 回调（VulkanRenderer renderpass 内调用）
    static void renderImGuiUi(VkCommandBuffer cmd);

    // 平台原生"另存为"对话框（Windows 资源管理器；PNG 过滤器）。
    // defaultName = 对话框默认文件名（可含或省略 .png）。返回用户选择的完整路径；
    // 返回空串 = 用户取消（调用方应中止导出、不生成文件）。
    // 内部记忆上次导出目录，下次默认定位到该目录。
    static std::string saveFileDialog(const char* defaultName);   // PNG 便捷包装
    // 通用"另存为"：filter 为 Win32 过滤器串（"说明\0通配\0…\0\0"），defExt 为默认扩展名
    static std::string saveFileDialogEx(const char* defaultName, const char* filter,
                                        const char* defExt);

    // 通用"打开文件"：filter 为 Win32 过滤器串（同 saveFileDialogEx），defExt 为默认
    // 扩展名（可为 nullptr）。返回用户选择的完整路径；空串 = 用户取消。
    // 用途：ESP 参数面板选择 density / ESP cube 文件。
    static std::string openFileDialogEx(const char* filter, const char* defExt);

private:
    bool createWindow();
    bool initImGuiBackends();
    static void newFrame(float dt);
    void render();                          // 菜单 + DockSpace + 所有面板 + 遮罩
    void renderMainMenuBar();
    void initWorkbench();               // 自绘标题栏（主方案 §7.3）
    void setupDocking();                    // Dock 布局（主方案 §7.4）
    void renderLoadingOverlay();            // 加载遮罩（主方案 §7.5）
    [[nodiscard]] bool windowIsMaximized() const;         // 真实窗口最大化状态（Win32 查询）

    // 帧时间观测（性能）：记录每帧实际耗时，每 kFrameStatsPeriodSec 秒打一条
    // 分布日志（avg/p50/p95/p99/max 与最低 FPS）。
    // 为什么需要它：moleculens_bench 测的是**引擎**渲染路径（headless、不含 ImGui/
    // Dock/面板），而用户感受到的是整帧（事件 → ImGui 全量重建 → 引擎 → 呈现）。
    // 两者差值就是 UI 层的成本，只有在这里量才拿得到。
    // dtMs = 帧**周期**（含末尾为限帧而睡的时间），workMs = 本帧实际工作量。
    // 两者必须分开看：周期接近限帧目标而工作量远小于它 → 是限帧在起作用（还有
    // 余量）；工作量已逼近周期 → 真的饱和了。只报周期会把"限帧"误读成"性能到头"。
    void recordFrameTime(float dtMs, float workMs);
    static constexpr size_t kFrameTimeWindow = 600;      // 约 10 s @60fps
    static constexpr double kFrameStatsPeriodSec = 5.0;

    std::vector<float> frameTimes_;
    std::vector<float> workTimes_;
    size_t frameTimeCursor_ = 0;
    double frameStatsLastLog_ = 0.0;
    // 启动时待导入的轨迹（见 setPendingTrajectory）
    std::string pendingTrajectory_;

    GLFWwindow* window_ = nullptr;
    bool windowShouldClose_ = false;
    bool maximized_ = false;   // 标题栏按钮图标用（真值以 chrome::windowIsMaximized 为准）
    // 自绘标题栏最大化状态机（还原矩形记录）——实现在 view/chrome/window_chrome.cpp
    chrome::MaximizeState maximizeState_;
    float lastFrameTime_ = 0.0f;
    float fps_ = 0.0f;
    // 自绘标题栏实际高度（renderMainMenuBar 每帧测量；setupDocking 用其偏移，
    // 避免硬编码 34 与字号/内边距变化后的真实高度不一致导致 DockSpace 偏移）
    float titleBarHeight_ = 34.0f;

    engine::VulkanRenderer* vk_ = nullptr;
    engine::Renderer* renderer_ = nullptr;
    engine::OrbitCamera* camera_ = nullptr;
    AppServices* svc_ = nullptr;
    DataManager* dataMgr_ = nullptr;
    TimelineController* timelineCtl_ = nullptr;
    IoController* ioCtl_ = nullptr;
    SelectionController* selectionCtl_ = nullptr;
    SceneController* sceneCtl_ = nullptr;
    RenderController* renderCtl_ = nullptr;
    RepresentationController* repCtl_ = nullptr;
    CameraController* cameraCtl_ = nullptr;

    // 工具面板观察指针（注册进 tools_ 注册表；svc* 注入控制器）
    MolListTool* molListTool_ = nullptr;
    StylesTool* stylesTool_ = nullptr;
    MaterialTool* materialTool_ = nullptr;
    RepsTool* repsTool_ = nullptr;


    MainView mainView_;
    MolScene molScene_;
    MultiView multiView_;   // 多视图（1/2/4 分屏；默认单视图，不改变历史行为）
    TimeAxis timeAxis_;
    ToolRegistry tools_;
    Workbench workbench_;   // v2 Aurora 工作台（固定五区 + 检查器模块）
    bool workbenchReady_ = false;   // 首帧延迟装配（控制器注入晚于 init）

    // 面板可见性（菜单控制）
    bool showMolList_ = true;
    bool showStyles_ = true;
    bool showMaterial_ = true;
    bool showExport_ = true;   // 图像导出面板默认可见（导出功能需面板触发）
    bool showPaint_ = false;
    bool showTimeline_ = true;
    bool showStatusBar_ = true;
    bool showLog_ = false;

    // 加载遮罩状态（主方案 §7.5）
    LoadingState loading_;
    bool loadingActive_ = false;

    // 日志面板状态（环形缓冲快照 + 自动滚动）
    std::vector<LogRecord> logSnapshot_;
    std::uint64_t logSeen_ = 0;      // 已取快照时的 totalPushed()，用于判新
    bool logAutoScroll_ = true;

    // 布局状态（M5 持久化）。
    // 初值必须是 false：setupDocking 仅在"未初始化"时构建默认 Dock 布局，
    // 而首次运行既没有 settings.json 也没有 imgui.ini，没有任何路径会把它置为
    // false —— 初值为 true 会让全新环境直接跳过默认布局，窗口全部漂浮重叠。
    // 有保存布局时 loadLayout() 同样置 false，行为不变。
    bool layoutInitialized_ = false;
};

} // namespace moleculens::app
