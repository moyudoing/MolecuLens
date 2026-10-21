// =============================================================================
// MolecuLens v2 · app/workbench/workbench.h —— 固定工作台（Aurora）
//
// 与 v1 的 ImGui DockSpace 不同：v2 用一块全屏窗口 + 手写五区布局
//   [标题栏] [菜单栏] [文件栏]
//   [左导航 92px][视口自适应][右检查器 300–440px]
//   [底部快捷条] [状态栏]
// 这样参数永远在固定位置，符合 Multiwfn 式"仪器面板"的心智模型，
// 也消除了 docking 布局错乱/面板漂浮/窄栏挤压等 v1 问题。
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <glm/glm.hpp>

#include "app/workbench/modules/module.h"
#include "engine/rep/representation.h"
#include "persist/mlscene/mlscene.h"   // Annotations（标注持久化）

namespace moleculens::entity {
class SceneStructure;
}
namespace moleculens::engine {
class VulkanRenderer;
class Renderer;
}
namespace moleculens::app {

class DataManager;
class MolScene;
class SceneController;
class RenderController;
class RepresentationController;
class TimelineController;
class CameraController;
class IoController;
class TimeAxis;

// 工作台运行所需的全部依赖（由 ViewControl 装配；工作台不拥有它们）
struct WorkbenchServices {
    engine::VulkanRenderer* vk = nullptr;
    engine::Renderer* renderer = nullptr;
    DataManager* data = nullptr;
    MolScene* viewport = nullptr;
    entity::SceneStructure* scene = nullptr;
    SceneController* sceneCtl = nullptr;
    RenderController* renderCtl = nullptr;
    RepresentationController* repCtl = nullptr;
    TimelineController* timelineCtl = nullptr;
    CameraController* cameraCtl = nullptr;
    IoController* ioCtl = nullptr;
    const float* fps = nullptr;

    // 无边框窗口控制（ViewControl 提供；未设置时相关按钮不生效）
    std::function<void()> requestClose;
    std::function<void()> minimizeWindow;
    std::function<void()> toggleMaximize;
    std::function<bool()> isMaximized;
    std::function<void(float, float)> dragWindow;

    // 原生文件对话框（返回空串 = 用户取消）
    std::function<std::string(const char* filter, const char* defExt)> dlgOpen;
    std::function<std::string(const char* defaultName, const char* filter,
                              const char* defExt)> dlgSave;
};

class Workbench {
public:
    Workbench();
    ~Workbench();

    void init(const WorkbenchServices& svc);
    void render();   // 每帧一次（在 ImGui::NewFrame 之后、Render 之前）

    // —— 模块服务（检查器模块的唯一入口）——
    [[nodiscard]] engine::VulkanRenderer* vk() const { return svc_.vk; }
    [[nodiscard]] engine::Renderer* renderer() const { return svc_.renderer; }
    [[nodiscard]] DataManager* data() const { return svc_.data; }
    [[nodiscard]] MolScene* viewport() const { return svc_.viewport; }
    [[nodiscard]] entity::SceneStructure* scene() const { return svc_.scene; }
    [[nodiscard]] RepresentationController* reps() const { return svc_.repCtl; }
    [[nodiscard]] SceneController* sceneCtl() const { return svc_.sceneCtl; }
    [[nodiscard]] RenderController* renderCtl() const { return svc_.renderCtl; }
    [[nodiscard]] TimelineController* timeline() const { return svc_.timelineCtl; }
    [[nodiscard]] CameraController* cameraCtl() const { return svc_.cameraCtl; }
    [[nodiscard]] IoController* io() const { return svc_.ioCtl; }
    [[nodiscard]] float fps() const;

    // —— 表示编辑目标（与表示控制器同一份 activeIndex，避免"两份状态"）——
    [[nodiscard]] int32_t activeRepIndex() const;
    void setActiveRepIndex(int32_t index);
    [[nodiscard]] engine::Representation* activeRep();
    [[nodiscard]] const std::vector<engine::Representation>& representations() const;

    // —— v2.9：表示行多选（Ctrl 点选 / Shift 连选）+ 批量操作 ——
    [[nodiscard]] bool isRepMultiSelected(int32_t id) const;
    void toggleRepMultiSelected(int32_t id);
    void clearRepMultiSelection() { repMultiSel_.clear(); }
    [[nodiscard]] const std::vector<int32_t>& repMultiSelection() const { return repMultiSel_; }

    // —— 常用动作 ——
    void notify(const std::string& text);
    void logLine(const std::string& text);
    [[nodiscard]] const std::vector<std::string>& logLines() const { return logLines_; }
    void clearLog() { logLines_.clear(); }
    void setModule(const char* id);
    [[nodiscard]] const char* activeModuleId() const;
    // 已注册的检查器模块数（关于框/文档里的"N 个模块"应与它一致，避免硬编码漂移）
    [[nodiscard]] size_t moduleCount() const { return modules_.size(); }
    [[nodiscard]] bool hydrogenHidden() const { return hideHydrogen_; }
    void setHydrogenHidden(bool on);
    // 分子显示视图叠加：坐标轴方向 / 比例尺与尺寸（用户可分别关闭）
    [[nodiscard]] bool showViewAxes() const { return showViewAxes_; }
    void setShowViewAxes(bool on) { showViewAxes_ = on; }
    [[nodiscard]] bool showViewScale() const { return showViewScale_; }
    void setShowViewScale(bool on) { showViewScale_ = on; }
    // 「当前分子」实例下标（分子面板里点选；新建表示时用它预填选择表达式）
    [[nodiscard]] int32_t selectedMolecule() const { return selMolecule_; }
    void setSelectedMolecule(int32_t index) { selMolecule_ = index; }
    void applyPreset(int preset);   // 见 StylePreset
    void frameCamera();
    void resetCamera();
    void openFileDialog();
    void saveSceneDialog();
    void importTrajectoryDialog();
    void exportPngDialog();
    // 原生文件对话框（模块用；工作台未注入 dlgOpen 时返回空串）
    [[nodiscard]] std::string chooseFile(const char* filter, const char* defExt);
    void toggleTheme();

    // 检查器宽度（右栏；拖拽或设置页调整，纯运行期状态）
    [[nodiscard]] float inspectorWidth() const { return inspectorW_; }
    void setInspectorWidth(float w);

    // 左导航收起（只剩图标，宽度 92 → 46 px；纯运行期状态）
    [[nodiscard]] bool railCollapsed() const { return railCollapsed_; }
    void setRailCollapsed(bool on);
    void toggleRailCollapsed();

    [[nodiscard]] bool hasDocument() const;
    [[nodiscard]] std::string documentName() const;
    [[nodiscard]] int totalAtoms() const;
    [[nodiscard]] int totalMolecules() const;
    // 分子质心（世界坐标；用于标签/箭头锚点）。越界返回 (0,0,0)。
    [[nodiscard]] glm::vec3 moleculeCenter(size_t moleculeIndex) const;
    // 当前视图中心（相机 target），用于"在视图中心放置标签"。
    [[nodiscard]] glm::vec3 viewCenter() const;

    // —— 场景绘制 / 标注（Publication / 示意图风格）——
    // 这些是**视图叠加层**：视口实时可见，且现在会一并栅格化进导出的 PNG
    //（engine/export/overlay/annotation/scene_overlay_raster.h）。
    struct SceneLabel {
        std::string text;
        glm::vec3   pos{0.0f};              // 世界坐标（Å）
        ImVec4      color{1, 1, 1, 1};
        float       size = 18.0f;           // 像素（按 900 px 高的视口标定，导出同比放大）
        bool        visible = true;
        bool        leader = true;          // 是否画到锚点的小圆点
    };
    struct SceneArrow {
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        ImVec4    color{1, 1, 1, 1};
        float     width = 2.2f;             // 线宽（像素）
        float     headScale = 1.0f;         // 箭头大小倍率
        bool      doubleHead = false;       // 两端都有箭头
        int       style = 0;                // 0 普通 / 1 波浪（光子"光"箭头）
        bool      visible = true;
    };
    // 水印：图角落的大号半透明文字（示意图底纹，如 "TCBG"）
    struct SceneWatermark {
        bool        visible = false;
        std::string text;
        ImVec4      color{1, 1, 1, 0.16f};
        float       scale = 0.10f;          // 字号 = 图像高度 × scale
        int         anchor = 0;             // 0 右下 / 1 左下 / 2 居中 / 3 右上
    };

    [[nodiscard]] bool showBox() const { return showBox_; }
    void setShowBox(bool on) { showBox_ = on; }
    [[nodiscard]] ImVec4 boxColor() const { return boxColor_; }
    void setBoxColor(const ImVec4& c) { boxColor_ = c; }
    // 手动盒（"位置水盒子"）：手动模式下盒中心/半长由用户给定，不再依赖轨迹 box
    [[nodiscard]] bool boxManual() const { return boxManual_; }
    void setBoxManual(bool on) { boxManual_ = on; }
    [[nodiscard]] glm::vec3 boxCenter() const { return boxCenter_; }
    void setBoxCenter(const glm::vec3& c) {
        boxCenter_ = c;
        boxHasRaw_ = false;   // 手动编辑即放弃文件里的三斜原始几何
    }
    [[nodiscard]] glm::vec3 boxHalf() const { return boxHalf_; }
    void setBoxHalf(const glm::vec3& h) {
        boxHalf_ = h;
        boxHasRaw_ = false;
    }
    [[nodiscard]] float boxLineWidth() const { return boxLineWidth_; }
    void setBoxLineWidth(float w) { boxLineWidth_ = w; }
    // 把盒中心/半长设为当前场景包围盒（"适配场景"按钮）
    void fitBoxToScene();

    [[nodiscard]] std::vector<SceneLabel>& labels() { return labels_; }
    [[nodiscard]] std::vector<SceneArrow>& arrows() { return arrows_; }
    [[nodiscard]] SceneWatermark& watermark() { return watermark_; }
    void addLabel(const std::string& text, const glm::vec3& pos);
    void labelMolecules(size_t first, size_t count);
    void clearAnnotations();
    // 出版风格：黑底 + 关阴影 + 平光材质（VMD 黑底卡通观感）
    void applyPublicationStyle();
    // 示意图风格：黑底 + 平光 + 强轮廓 + 关阴影 + 无距离淡出（参考 TCBG 光合体系图）
    void applyDiagramStyle();
    // 每帧把标注写入 engine 快照，供 PNG 导出栅格化（视口绘制直接从成员读）
    void publishOverlaySnapshot();

    // —— 标注持久化（v2.3）：与 .mlscene 的 "annotations" 键互转（纯数据）——
    // 保存时由 IoController 通过桥回调取走；载入时回填到本工作台。
    [[nodiscard]] persist::mlscene::Annotations exportAnnotations() const;
    void importAnnotations(const persist::mlscene::Annotations& annotations);

    // 快捷样式预设（底部快捷条 / 视口工具条共用）
    enum StylePreset : int {
        kSobArt = 0,
        kIBOview = 1,
        kHoukMol = 2,
        kIQmol = 3,
        kClearStyle = 4,
        kPresetCount = 5,
    };

private:
    void renderTitleBar(const ImVec2& pos, const ImVec2& size);
    void renderMenuBar(const ImVec2& pos, const ImVec2& size);
    void renderFileBar(const ImVec2& pos, const ImVec2& size);
    void renderRail(const ImVec2& pos, const ImVec2& size);
    void renderViewport(const ImVec2& pos, const ImVec2& size);
    void renderInspector(const ImVec2& pos, const ImVec2& size);
    void renderQuickBar(const ImVec2& pos, const ImVec2& size);
    void renderStatusBar(const ImVec2& pos, const ImVec2& size);
    void renderToast();
    void renderModals();
    // 保存场景（链接式 / 嵌入式轨迹）选择框；确认后再弹系统另存为。
    // mode：0 = 链接式，1 = 嵌入式（与界面单选一致；此处用 int 避免头文件引入
    // IoController 完整类型，形成 app 内部不必要的头文件依赖）。
    void renderSaveSceneModal();
    void doSaveScene(int mode);

    void addModule(std::unique_ptr<IInspectorModule> module);
    IInspectorModule* findModule(const char* id) const;
    // 计算当前水盒几何（文件读回的三斜盒 / 手动中心+半长 / 轨迹首帧 box）。
    // 返回 false = 当前没有可画的盒。
    bool currentBox(glm::vec3* origin, glm::vec3* a1, glm::vec3* a2, glm::vec3* a3) const;
    // 把世界坐标投影到视口像素坐标；返回 false = 在相机背后/裁剪外
    bool projectToViewport(const glm::vec3& world, const ImVec2& imgMin,
                           const ImVec2& imgMax, ImVec2* out) const;
    void drawSceneOverlay(const ImVec2& imgMin, const ImVec2& imgMax);

    WorkbenchServices svc_;
    std::vector<std::unique_ptr<IInspectorModule>> modules_;
    std::string activeId_ = "visualize";
    std::string pendingId_;

    std::vector<int32_t> repMultiSel_;  // v2.9：表示行多选集合（Ctrl/Shift 点选）
    bool showMolLabels_ = false;      // 视口内是否标注分子名称
    bool showViewAxes_ = true;        // 视口左下坐标轴方向指示
    bool showViewScale_ = true;       // 视口比例尺 + 场景尺寸（Å）
    bool inspectorSplitHot_ = false;   // 分隔条悬停/拖动中
    int themeDarkCached_ = -1;         // 主题明暗缓存（-1 = 尚未同步过背景色）
    float inspectorW_ = 500.0f;   // 与 aurora::metrics::kInspector 一致（可拖动分隔条改）
    bool railCollapsed_ = false;  // 左导航收起态（92 → 46 px，只剩图标）
    bool hideHydrogen_ = false;
    int32_t selMolecule_ = -1;
    std::map<int32_t, std::string> savedSelections_;
    int preset_ = -1;

    // 时间轴控件（v2.7）：导入轨迹后出现在分子视图正下方；控制器每帧同步
    std::unique_ptr<TimeAxis> timeAxis_;

    // 提示气泡（3.2 秒淡出）
    std::string toast_;
    float toastAge_ = 999.0f;

    // 运行日志（供「日志」模块显示；上限 400 条）
    std::vector<std::string> logLines_;

    // 导出对话框
    bool showExport_ = false;
    int exportScale_ = 2;
    int exportWidth_ = 1920;
    int exportHeight_ = 1080;
    bool exportTransparent_ = false;

    // 保存场景对话框（双模式）：0 = 链接式（默认），1 = 嵌入式。
    // 选择会写入 settings.json 的 sceneSaveMode，下次打开对话框时沿用。
    bool showSaveScene_ = false;
    int saveSceneMode_ = 0;

    bool showHelp_ = false;
    bool firstFrame_ = true;

    // 场景绘制/标注（视图叠加层）
    bool showBox_ = false;
    ImVec4 boxColor_{0.45f, 0.62f, 0.85f, 0.9f};
    bool boxManual_ = false;             // false = 跟随轨迹首帧 box；true = 手动中心/半长
    glm::vec3 boxCenter_{0.0f};
    glm::vec3 boxHalf_{10.0f};
    float boxLineWidth_ = 1.4f;
    // 从 .mlscene 读回的盒：可能是三斜盒，无法用"中心+半长"表示，故原样保留
    // 三条棱向量；用户一旦在界面上改中心/半长，就切回手动轴对齐（清 raw）。
    bool boxHasRaw_ = false;
    glm::vec3 boxRawOrigin_{0.0f};
    glm::vec3 boxRawA1_{1.0f, 0.0f, 0.0f};
    glm::vec3 boxRawA2_{0.0f, 1.0f, 0.0f};
    glm::vec3 boxRawA3_{0.0f, 0.0f, 1.0f};
    std::vector<SceneLabel> labels_;
    std::vector<SceneArrow> arrows_;
    SceneWatermark watermark_;

    // 视口悬浮条矩形（上一帧），用于把相机拖拽和按钮点击隔开
    std::vector<ImVec4> vpOverlayRects_;
    ImVec2 viewportMin_{0, 0};
    ImVec2 viewportMax_{0, 0};
};

// 模块工厂（定义在 modules.cpp；新增模块只需在此加一行声明 + 实现）
std::unique_ptr<IInspectorModule> makeVisualizeModule();
std::unique_ptr<IInspectorModule> makeSceneModule();
std::unique_ptr<IInspectorModule> makeEspModule();
std::unique_ptr<IInspectorModule> makeOrbitalModule();
std::unique_ptr<IInspectorModule> makeTrajectoryModule();
std::unique_ptr<IInspectorModule> makeAnalysisModule();
std::unique_ptr<IInspectorModule> makeLogModule();
std::unique_ptr<IInspectorModule> makeSettingsModule();
std::unique_ptr<IInspectorModule> makeAnnotationModule();

} // namespace moleculens::app
