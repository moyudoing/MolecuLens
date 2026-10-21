// =============================================================================
// MolecuLens · engine · render/renderer.h —— Renderer（prepare/draw 编排）
// 规格来源：主方案 §5.4 / §5.8（Stage1 并行生成命令流；Stage2 后端执行；
//           脏位域驱动；serial 缓存键 P5）
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/rep/representation.h"
#include "engine/render/render_parameter.h"   // LightParams
#include "entity/scene/scene.h"   // FrameData 完整类型（值成员 frameData_）

namespace moleculens::entity {
class SceneStructure;
} // namespace moleculens::entity

namespace moleculens::engine {

class VulkanRenderer;
class OffscreenRenderer;

// 渲染视图（draw 输入：相机 + 视口尺寸 + 光照 + 雾）
struct RenderView {
    OrbitCamera camera;
    uint32_t width = 1, height = 1;
    float dt = 0.0f;
    LightParams light;                          // 光照（默认参数；应用层从场景属性填装）
    RenderStyle style;                          // 风格（柔和光/描边/距离淡出；渲染器持有）
    glm::vec3 backgroundColor{0.05f, 0.05f, 0.08f};  // 清屏/淡出目标色（线性）
    // 剖切面（世界空间 dot(p,n)+d=0；保留 <=0 一侧）。仅进 UBO，不重建命令流。
    glm::vec4 clipPlane{1.0f, 0.0f, 0.0f, 0.0f};
    bool clipEnabled = false;
    glm::vec4 fog{0.0f, 0.0f, 0.0f, 0.05f};     // 雾：x near, y far, z mode(0关), w density
    // 全局缩放（SceneProperties::scale → UBO sceneProps.x）。着色器把世界坐标整体
    // 乘该系数（绕世界原点等比缩放，含原子半径/键长），相机不动。
    // 1.0 = 原尺寸；>1 放大（可能超出相机取景框），<1 缩小。
    float scale = 1.0f;
    // 动态帧（轨迹播放 / 拖时间轴）：用低细分球/柱并跳过阴影 pass，保证播放
    // 跟手。静止（暂停/不拖）时自动回到高细分 + 阴影，画面质量不受损。
    bool lowDetail = false;
};

class Renderer {
public:
    ~Renderer();

    // 场景指针（Renderer 负责置脏/清脏，主方案 §5.4 prepare 阶段）
    void setScene(entity::SceneStructure* scene) { scene_ = scene; }
    [[nodiscard]] const entity::SceneStructure* scene() const { return scene_; }
    // 可变场景访问：动画导出需要逐帧改写坐标段（applyTrajectoryFrame）后重建命令流。
    // 单独命名而非重载 const 版本，避免既有调用点的重载解析被意外改变。
    [[nodiscard]] entity::SceneStructure* mutableScene() { return scene_; }
    void setVulkanRenderer(VulkanRenderer* vk) { vulkan_ = vk; }

    // 脏位分类（主方案 §5.4 / §5.8.2 脏位域 → 重建粒度）：
    //   DIRTY_STRUCTURE / DIRTY_INSTANCE / DIRTY_REPRESENTATION → 整条命令流重建
    //   DIRTY_COORDS（仅帧切）                                   → 只重放坐标段
    //   DIRTY_PROPERTIES（背景/光照/雾/缩放）                     → 无需重建（只进 UBO）
    void update(float dt);
    void prepare();                       // ★ Stage1: 生成命令流（按脏位选重建粒度）
    void draw(const RenderView& view);    // ★ Stage2: 命令流 → 后端执行

    // —— 表面生成异步化（GUI 主线程；REMAINING_WORK §1.1）——
    // 开启后整体系 QuickSurf 在后台生成：该表面尚未就绪时主线程**保留上一次命令流**
    // （旧画面继续显示）并立即返回，不再阻塞约 197 ms；后台任务完成后下一次 prepare
    // 自动重建并把新表面换入。关闭（默认）时行为与既有一致（同步生成），
    // CLI / 导出 / 单元测试都不受影响。
    void setAsyncSurfaceEnabled(bool enabled);
    [[nodiscard]] bool asyncSurfaceEnabled() const {
        return asyncSurface_.load(std::memory_order_relaxed);
    }
    // 是否正在等待后台表面生成（视口显示"生成中…"、面板提示用）。
    [[nodiscard]] bool surfaceGenerationPending() const;

    // 导出线程专用：先等未完成的表面任务写完缓存，再同步重建命令流；
    // 返回时命令流已包含最新表面（导出不会被"生成中"的旧画面污染）。
    void prepareBlocking();

    [[nodiscard]] const SceneCommandList& commandList() const { return commandList_; }
    SceneCommandList& commandList() { return commandList_; }
    [[nodiscard]] uint64_t commandSerial() const { return commandList_.serial(); }
    // 上一次 prepare 是否走了坐标重放（诊断/测试用）
    [[nodiscard]] bool lastPrepareWasReplay() const { return lastWasReplay_; }

    // —— 表示列表（场景级）——
    // 引擎**不拥有**表示数据，只持只读视图 + 修订号。此前按值持有整表副本，
    // 视图层每次变更（含滑杆逐帧拖动）都要深拷贝一次（元素含 2 个 std::string）。
    // 契约：调用方保证 *reps 在 unbindRepresentations() 或 Renderer 析构前有效。
    void bindRepresentations(const std::vector<Representation>* reps, uint64_t revision);
    void unbindRepresentations();
    [[nodiscard]] const std::vector<Representation>& representations() const;

    // 帧数据（当前帧坐标段；由外部写 CoordsSoA 后刷新）
    void refreshFrameData();

    // 场景包围球（自动取景用）
    bool sceneBounds(glm::vec3* outCenter, float* outRadius) const;

    // —— 视图参数组装（视图层唯一入口，取代原先各处手写 4~5 行）——
    // 场景光照（天顶/地面半球色 + 环境光）与雾：从场景属性读取；无场景时给默认值。
    // 非光栅化路径（CPU 光追预览/导出）需直接取用，故单独暴露。
    [[nodiscard]] LightParams sceneLighting() const;
    [[nodiscard]] glm::vec4 sceneFog() const;

    // 渲染风格（柔和光/描边/距离淡出）：纯 UBO 参数，不触发命令流重建。
    // 默认构造 = RenderStyle 默认值；GUI 预设/CLI --style 都写这里。
    void setRenderStyle(const RenderStyle& style) { style_ = style; }
    [[nodiscard]] const RenderStyle& renderStyle() const { return style_; }

    // 一次渲染所需的完整视图参数（相机 + 视口尺寸 + 光照/雾）。
    [[nodiscard]] RenderView viewFor(const OrbitCamera& camera, uint32_t width,
                                     uint32_t height) const;

private:
    // 命令流生成。replay = true 时只重跑各表示的坐标数学（不重发材质/渲染模式
    // 等状态命令），把载荷写回上次记录的偏移——见 SceneCommandList::beginReplay。
    // 返回 false = 本轮遇到"表面正在后台生成"：新命令流被丢弃、旧命令流保留，
    // 待任务完成后再次重建（调用方据此维持 needsRebuild_）。
    bool rebuildCommandList(bool replay);

    entity::SceneStructure* scene_ = nullptr;
    VulkanRenderer* vulkan_ = nullptr;
    SceneCommandList commandList_;
    entity::FrameData frameData_;
    const std::vector<Representation>* representations_ = nullptr;   // 非拥有
    uint64_t repsRevision_ = 0;      // 绑定时的表示修订号（变更即触发重建）
    bool needsRebuild_ = true;       // 结构/实例/表示变更 → 整条命令流重建
    bool needsCoords_ = false;       // 仅坐标变更 → 尝试坐标重放
    bool lastWasReplay_ = false;     // 上次 prepare 实际走了重放路径

    // —— 异步表面生成状态 ——
    std::atomic<bool> asyncSurface_{false};       // 异步模式（GUI 打开）
    std::atomic<bool> waitingForSurface_{false};  // 正在等后台表面结果（保留旧命令流）
    bool forceSyncRebuild_ = false;               // 本次重建强制同步（导出线程用）
    RenderStyle style_{};                         // 渲染风格（UBO only；默认值）
    std::chrono::steady_clock::time_point surfaceWaitStart_{};   // 等待起点（诊断日志用）
    mutable std::mutex prepareMutex_;             // 串行化主线程/导出线程的 prepare
};

} // namespace moleculens::engine
