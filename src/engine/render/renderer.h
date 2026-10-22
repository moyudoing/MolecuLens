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
class GpuRayTracer;
struct RtScene;
struct RtCamera;
struct RtLight;

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
    // 构造/析构都在 .cpp 定义：成员里持有 std::unique_ptr<GpuRayTracer>（前向声明），
    // 内联默认构造会在异常清理路径上实例化 default_delete → 需要完整类型。
    Renderer();
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
    // 多视图：场景脏位由场景全局持有，先渲染的视图会在 rebuildCommandList 末尾
    // 清脏 —— 其它视图若之后再调 update() 就看不到本帧变更（命令流永不重建）。
    // 因此多视图外层在整帧开始时抓一份脏位快照，再用本入口显式喂给每个视图；
    // 单视图 / CLI / 测试仍走 update(dt)（等价于 updateWithDirty(dt, 场景脏位)）。
    void updateWithDirty(float dt, uint32_t dirty);
    // 当前轨迹帧号：表示生成/表面预取用（不影响命令流内容，只作缓存键）。
    // 由 MolScene 每帧从时间轴同步；无轨迹或静态场景可保持 0。
    void setTrajectoryFrame(uint64_t frame) { frameIndex_ = frame; }
    [[nodiscard]] uint64_t trajectoryFrame() const { return frameIndex_; }
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

    // —— 运动中冻结坐标相关的表面几何（轨迹播放/拖时间轴）——
    // 冻结时 QuickSurf/体系 vdW 表面直接复用上一张网格，不再逐帧重算（md.gro 实测
    // 单次 ~730 ms，不冻结会把播放拖到 1–3 FPS）。球棍/线框仍逐帧更新；解冻时
    // 自动触发一次重建，把表面刷到当前帧。
    void setSurfaceFreeze(bool frozen);
    [[nodiscard]] bool surfaceFrozen() const {
        return freezeSurfaces_.load(std::memory_order_relaxed);
    }

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

    // —— 多视图：每个视图独立的显示筛选（只影响命令流，不改场景数据）——
    // instanceMask：每实例 0/1（空 = 全部实例）；repMask：每表示 0/1（空 = 全部表示）。
    // 与表示自身的实例级类型范围（Representation::rangeMode）叠加：两者都通过才生成
    // 命令；整体系表面（systemSurface）也会按实例掩码裁掉被筛掉的体系 —— 因此同一
    // 文档里的不同文件/体系可以在不同视图里各显示各的。
    // 掩码按值复制保存，调用方不必保持向量生命周期。变更会触发一次命令流重建。
    void setPaneFilter(std::vector<uint8_t> instanceMask, std::vector<uint8_t> repMask);
    void clearPaneFilter() { setPaneFilter({}, {}); }
    [[nodiscard]] bool hasPaneFilter() const {
        return !paneInstanceMask_.empty() || !paneRepMask_.empty();
    }

    // 帧数据（当前帧坐标段；由外部写 CoordsSoA 后刷新）
    void refreshFrameData();

    // 场景包围球（自动取景用）
    bool sceneBounds(glm::vec3* outCenter, float* outRadius) const;

    // —— GPU 光追（Vulkan KHR acceleration structure + ray tracing pipeline）——
    // 懒初始化 GpuRayTracer：GUI 复用主设备（setVulkanRenderer 注入），CLI/测试
    // 无主设备时自建 headless 设备。设备不支持硬件光追时返回 false —— 调用方
    // 回退 CPU 光追（导出/CLI 均有回退，不会因为无 RT 而失败）。
    // 输出契约与 CPU rayTrace() 一致：RGBA8，行序 top→bottom。
    bool gpuRayTraceAvailable();
    bool gpuRayTrace(const RtScene& scene, const RtCamera& camera,
                     const RtLight& light, const glm::vec3& backgroundColor,
                     int samplesPerPixel, std::vector<uint8_t>& outRgba);

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
    uint64_t meshSerialCounter_ = 0; // 网格内容修订号计数（仅全量重建递增）
    // 多视图筛选（空 = 不过滤；见 setPaneFilter）
    std::vector<uint8_t> paneInstanceMask_;
    std::vector<uint8_t> paneRepMask_;
    bool needsRebuild_ = true;       // 结构/实例/表示变更 → 整条命令流重建
    bool needsCoords_ = false;       // 仅坐标变更 → 尝试坐标重放
    bool lastWasReplay_ = false;     // 上次 prepare 实际走了重放路径
    uint64_t frameIndex_ = 0;       // 时间轴当前帧（表面预取的缓存键）

    // —— 异步表面生成状态 ——
    std::atomic<bool> asyncSurface_{false};       // 异步模式（GUI 打开）
    std::atomic<bool> waitingForSurface_{false};  // 正在等后台表面结果（保留旧命令流）
    std::atomic<bool> freezeSurfaces_{false};     // 运动中冻结坐标相关表面
    bool forceSyncRebuild_ = false;               // 本次重建强制同步（导出线程用）
    RenderStyle style_{};                         // 渲染风格（UBO only；默认值）
    // —— GPU 光追（懒初始化；失败后不再重试，直接走 CPU 回退）——
    std::unique_ptr<GpuRayTracer> gpuRt_;
    std::mutex gpuRtMutex_;                       // 导出线程/主线程串行初始化
    std::chrono::steady_clock::time_point surfaceWaitStart_{};   // 等待起点（诊断日志用）
    mutable std::mutex prepareMutex_;             // 串行化主线程/导出线程的 prepare
};

} // namespace moleculens::engine
