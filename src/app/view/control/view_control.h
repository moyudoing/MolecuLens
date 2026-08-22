// =============================================================================
// MolecuLens · app · view/control/view_control.h —— 顶层视图控制器
// 规格来源：主方案 §7.1/§7.2/§7.3/§7.4/§7.5（窗口生命周期、ImGui 上下文、
//           主循环、Dock 布局、面板可见性开关、自绘标题栏、加载遮罩状态机）
// =============================================================================

#pragma once

#include <functional>
#include <memory>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder 系列
#include <volk.h>             // VkCommandBuffer（renderImGuiUi 静态回调签名）

#include "app/view/container/main_view.h"
#include "app/view/timeline/time_axis.h"   // TimeAxis 值成员（完整类型）
#include "app/view/tool/tool_registry.h"
#include "app/view/viewport/mol_scene.h"

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
class ExportImgTool;
class PaintingTool;
class TimeAxis;
class TimelineController;
class IoController;
class SelectionController;
class RenderController;
class RepresentationController;

// 加载遮罩状态机（主方案 §7.5）
enum class LoadingStage { Queued, ReadingFile, AnalyzingBonds, UploadingGPU, Done, Failed };
struct LoadingState {
    LoadingStage stage = LoadingStage::Done;
    std::string  fileName;
    uint64_t     fileSize = 0;
    float        progress = -1.0f;      // <0 表示不确定进度
    int          delayFrames = 0;       // 完成/失败后的关闭倒计时
};

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
    std::string serializeLayout();
    bool deserializeLayout(const std::string& base64);

    // 加载遮罩（主方案 §7.5）
    void beginLoading(const std::string& fileName, uint64_t fileSize);
    void updateLoading(const LoadingState& state);

    // 窗口控制（自绘标题栏，主方案 §7.3）
    void toggleMaximize();

    GLFWwindow* window() const { return window_; }
    bool windowShouldClose() const { return windowShouldClose_; }
    MolScene& molScene() { return molScene_; }
    TimeAxis& timeAxis() { return timeAxis_; }
    ToolRegistry& tools() { return tools_; }

    // 控制器服务注入（Application 装配）
    void svcTimeline(TimelineController* ctl) { timelineCtl_ = ctl; }
    void svcIo(IoController* ctl) { ioCtl_ = ctl; }
    void svcSelection(SelectionController* ctl) { selectionCtl_ = ctl; }
    void svcRender(RenderController* ctl);
    void svcRep(RepresentationController* ctl);

    // UI 回调（VulkanRenderer renderpass 内调用）
    static void renderImGuiUi(VkCommandBuffer cmd);

private:
    bool createWindow();
    bool initImGuiBackends();
    void newFrame(float dt);
    void render();                          // 菜单 + DockSpace + 所有面板 + 遮罩
    void renderMainMenuBar();               // 自绘标题栏（主方案 §7.3）
    void setupDocking();                    // Dock 布局（主方案 §7.4）
    void renderLoadingOverlay();            // 加载遮罩（主方案 §7.5）

    GLFWwindow* window_ = nullptr;
    bool windowShouldClose_ = false;
    bool maximized_ = false;
    float lastFrameTime_ = 0.0f;
    float fps_ = 0.0f;

    engine::VulkanRenderer* vk_ = nullptr;
    engine::Renderer* renderer_ = nullptr;
    engine::OrbitCamera* camera_ = nullptr;
    AppServices* svc_ = nullptr;
    DataManager* dataMgr_ = nullptr;
    TimelineController* timelineCtl_ = nullptr;
    IoController* ioCtl_ = nullptr;
    SelectionController* selectionCtl_ = nullptr;

    MainView mainView_;
    MolScene molScene_;
    TimeAxis timeAxis_;
    ToolRegistry tools_;

    // 面板可见性（菜单控制）
    bool showMolList_ = true;
    bool showStyles_ = true;
    bool showMaterial_ = true;
    bool showExport_ = false;
    bool showPaint_ = false;
    bool showTimeline_ = true;
    bool showStatusBar_ = true;
    bool showLog_ = true;

    // 加载遮罩状态（主方案 §7.5）
    LoadingState loading_;
    bool loadingActive_ = false;

    // 布局状态（M5 持久化）
    bool layoutInitialized_ = false;
};

} // namespace moleculens::app
