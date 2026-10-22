// =============================================================================
// MolecuLens v2 · app/view/container/multi_view.h —— 多视图（1 / 2 / 4 分屏对照）
//
// 需求（2026-10-22）：中心视口可以切成两个或四个**互相独立**的视图，各自放置
// 不同的分子文件/体系、各自旋转缩放，方便对照。
//
// 实现要点（为什么不复制整份场景文档）：
//   · 文档（SceneStructure）仍然是唯一一份：一次「打开文件」= 追加一个体系
//     （SystemGroup，名字 = 文件名），所有视图共享同一份原子/键/轨迹数据。
//   · 每个视图有自己的相机 + 自己的离屏渲染器（Renderer + MolScene）：
//       - 主视图直接复用工作台既有的 MolScene / Renderer（导出、表示编辑、
//         选择、标注等既有链路一行都不用改）；
//       - 副视图各持一个 Renderer，绑定同一场景/同一份表示列表（零拷贝）。
//   · 视图之间的"内容独立"由 Renderer::setPaneFilter 实现：实例掩码（按体系/
//     文件筛）+ 表示掩码（不同视图显示不同样式）。命令流按视图各自生成，
//     场景数据不被修改（分子列表/保存/导出看到的仍是完整文档）。
//   · 场景脏位是全局的，先渲染的视图会在 prepare 里清脏；本类在整帧开始时抓
//     快照，用 MolScene::render(size, hint) 喂给副视图（否则副视图永不刷新）。
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/rep/representation.h"

namespace moleculens::engine {
class OrbitCamera;
class Renderer;
class VulkanRenderer;
} // namespace moleculens::engine

namespace moleculens::entity {
class SceneStructure;
} // namespace moleculens::entity

namespace moleculens::app {

class MolScene;

// 布局（枚举值写进设置/菜单时用稳定整数，见 viewLayout()/setViewLayout）
enum class ViewLayout : int {
    Single = 0,        // 单视图（默认，保持历史行为）
    SideBySide = 1,    // 双视图 · 左右
    Stacked = 2,       // 双视图 · 上下
    Quad = 3,          // 四视图（2×2）
};

class MultiView {
public:
    MultiView();
    ~MultiView();

    // primary：工作台既有视口（主视图），不拥有。primaryCamera/primaryRenderer
    // 必须与它的内部指针一致（主视图筛选要直接下发到主渲染器）。
    void init(engine::VulkanRenderer* vk, engine::Renderer* primaryRenderer,
              entity::SceneStructure* scene, MolScene* primary,
              engine::OrbitCamera* primaryCamera);
    void shutdown();
    // 释放所有视图注册到 ImGui 描述符池的纹理。**必须在 ImGui 后端 Shutdown
    // 之前调用**（描述符池随后端销毁，之后释放会操作已销毁的池）。
    void releaseImGuiTextures();

    [[nodiscard]] ViewLayout layout() const { return layout_; }
    void setLayout(ViewLayout layout);
    [[nodiscard]] int paneCount() const;
    [[nodiscard]] bool multiPane() const { return layout_ != ViewLayout::Single; }

    [[nodiscard]] int activePane() const { return activePane_; }
    void setActivePane(int index);

    // —— 每帧渲染 ——
    // 返回主视图（视图 1）的图像矩形：工作台要用它画标注/箭头/工具叠加层，
    // 并在该矩形上处理工具点击。rendered=false 表示本帧没有可画视图。
    struct RenderResult {
        bool rendered = false;
        ImVec2 primaryMin{0.0f, 0.0f};
        ImVec2 primaryMax{0.0f, 0.0f};
    };
    RenderResult render(const ImVec2& pos, const ImVec2& size,
                        const std::vector<engine::Representation>& reps,
                        uint64_t repsRevision, bool showAxes, bool showScale,
                        uint64_t trajectoryFrame);

    // 给某个视图导入文件：等异步解析完成、场景里出现该名字的体系后，把该视图
    // 的筛选取景切到它（baseSystemCount = 导入前的体系数，用来只认新体系）。
    void assignNextSystemToPane(int pane, const std::string& name,
                                size_t baseSystemCount);
    [[nodiscard]] size_t systemCount() const;

    // 相机操作（对当前布局的所有视图各自生效，参数是视图下标）
    void framePane(int pane);          // 只适配可见内容，保持朝向
    void resetPaneCamera(int pane);    // 朝向复位 + 适配可见内容
    void copyPrimaryCameraTo(int pane);
    void frameAllPanes();              // 每个视图按自己的可见内容取景
    void copyPrimaryCameraToAll();      // 主视图相机复制到全部视图

    // 同步相机：开启后任一视图的旋转/缩放/平移会应用到所有视图（对照同一
    // 构象的不同表示时常用；默认关闭 = 各视图完全独立）。
    void setSyncCameras(bool on);
    [[nodiscard]] bool syncCameras() const { return syncCameras_; }

    // 视图标题条上的动作回调（由工作台注入：打开文件对话框 / toast 提示）
    void setOpenFileHandler(std::function<void(int)> fn) {
        openFileHandler_ = std::move(fn);
    }
    void setNotifyHandler(std::function<void(const std::string&)> fn) {
        notifyHandler_ = std::move(fn);
    }

    // 某视图当前显示范围是否被筛选（工作台状态栏可显示"视图 N：已筛选"）
    [[nodiscard]] bool paneFiltered(int pane) const;
    [[nodiscard]] std::string paneFilterLabel(int pane) const;
    // 某视图是否显示某个实例（工作台的"分子名"标注用：多视图下只标注该视图
    // 实际显示的分子，避免把被筛掉的文件名也画上去）。pane 越界 → true。
    [[nodiscard]] bool paneShowsInstance(int pane, uint32_t instanceIndex) const;

private:
    struct Pane {
        bool primary = false;
        MolScene* molScene = nullptr;             // 非拥有
        engine::OrbitCamera* camera = nullptr;    // 非拥有
        // 副视图独占的渲染资源（主视图为空，复用工作台既有对象）
        std::unique_ptr<engine::Renderer> renderer;
        std::unique_ptr<engine::OrbitCamera> ownCamera;
        std::unique_ptr<MolScene> ownScene;

        // —— 显示筛选（只影响本视图命令流，不改场景数据）——
        bool allSystems = true;             // true = 显示全部体系/文件
        std::vector<uint32_t> systemIds;    // allSystems=false 时生效
        std::vector<uint8_t> repMask;       // 空 = 显示全部表示
        // 上一帧实际下发到渲染器的掩码（用于判断"筛选变了"→ 置脏重建）
        std::vector<uint8_t> lastInstMask;
        std::vector<uint8_t> lastRepMask;

        // 相机拖拽状态（与 MolScene::handleMouse 的"按下不锁键"语义一致）
        int dragButton = -1;
        bool dragging = false;

        ImVec2 imageMin{0.0f, 0.0f};
        ImVec2 imageMax{0.0f, 0.0f};
    };

    Pane& ensurePane(size_t index);
    void resetPaneResources(Pane& pane);
    // 把 Pane 的筛选状态翻译成 Renderer 掩码；返回 true = 掩码有变化。
    // instanceChanged（可选）单独告诉调用方"实例掩码变了"——系统/文件筛选变化
    // 后需要自动适配相机，而只切换表示（样式）不需要动相机。
    bool applyPaneFilter(Pane& pane,
                         const std::vector<engine::Representation>& reps,
                         bool* instanceChanged = nullptr);
    // 视图标题条 / 边框 / 筛选菜单（画在图像之上，但标题条在图像区域之外）
    void drawPaneChrome(Pane& pane, int index, const ImVec4& rect);
    void drawPaneContentMenu(Pane& pane, int index);
    void drawPaneRepMenu(Pane& pane, int index,
                         const std::vector<engine::Representation>& reps);
    // 相机交互：左键轨道 / 中键推拉 / 右键滚转 / Shift+右键平移 / 滚轮缩放
    void handlePaneInput(Pane& pane, int index);
    // 同步相机：把 src 的相机复制到其它已创建的视图
    void propagateCamera(int src);
    // 只适配本视图可见实例的包围球
    bool paneVisibleBounds(const Pane& pane, glm::vec3* center, float* radius) const;
    [[nodiscard]] bool instanceVisibleInPane(const Pane& pane, uint32_t index) const;
    void consumePendingAssignment();

    engine::VulkanRenderer* vk_ = nullptr;
    engine::Renderer* primaryRenderer_ = nullptr;
    entity::SceneStructure* scene_ = nullptr;
    MolScene* primary_ = nullptr;
    engine::OrbitCamera* primaryCamera_ = nullptr;

    ViewLayout layout_ = ViewLayout::Single;
    std::vector<std::unique_ptr<Pane>> panes_;   // panes_[0] = 主视图
    int activePane_ = 0;

    std::function<void(int)> openFileHandler_;
    std::function<void(const std::string&)> notifyHandler_;

    // 导入文件 → 自动切到该体系的等待状态
    int pendingPane_ = -1;
    std::string pendingSystemName_;
    size_t pendingBaseCount_ = 0;
    int pendingFrames_ = 0;

    // 布局切换后的一帧：所有视图按各自内容重新取景（单视图 → 全场景）。
    // 否则从"视图 1 只看某个文件"切回单视图时，相机会停在那个小分子上，
    // 其余分子虽已恢复显示却在画面外，看起来像"文件丢了"。
    bool refitOnLayoutChange_ = false;
    bool syncCameras_ = false;
    // 本帧表示列表副本：标题条 popup 在 render() 之后才展开，需要能列出表示名。
    // 表示数量很小（个位数到几十），每帧一次 vector 拷贝可接受。
    std::vector<engine::Representation> cachedReps_;

    // 标题条高度（像素）：图像从标题条下方开始，避免按钮压住画面
    static constexpr float kHeaderH = 26.0f;
    // 视图之间的缝隙
    static constexpr float kGap = 6.0f;
};

} // namespace moleculens::app
