// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.h —— 3D 视口封装
// 规格来源：主方案 §7.7（离屏 FBO → ImGui::Image；相机交互：左键轨道/中键缩放/
//           右键平移/滚轮 dolly；getDisplayDevice 渲染参数出口）
// =============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include <glm/glm.hpp>
#include <imgui.h>
#include <volk.h>

#include "spdlog/spdlog.h"

#include "engine/raytrace/ray_tracer.h"   // RtScene / RtAccel（RT 级 LOD 缓存）

namespace moleculens::engine {
class Renderer;
class OrbitCamera;
class VulkanRenderer;
class OffscreenRenderer;
} // namespace moleculens::engine

namespace moleculens::entity {
class SceneStructure;
} // namespace moleculens::entity

namespace moleculens::app {

class CameraController;

class MolScene {
public:
    MolScene();      // 定义于 cpp（unique_ptr<OffscreenRenderer> 成员需完整类型）
    ~MolScene();

    // 初始化（命令流共享；离屏渲染器独占本类；vk = 主窗口 VulkanRenderer，
    // 离屏 FBO 建在其设备上 → ImGui 才能同设备采样纹理）
    void init(engine::Renderer* renderer, engine::OrbitCamera* camera,
              engine::VulkanRenderer* vk);

    // 相机控制器注入：视口的鼠标交互经它转发，使 CameraController 成为改变相机
    // 状态的唯一入口。注意**必须**在 init 之后由 ViewControl::svcCamera 补注入
    // （Application 里 ViewControl::init 早于控制器装配，构造期拿不到控制器）。
    // 未注入时 handleMouse/handleScroll 直接返回，不会绕过控制器直接写相机。
    void setCameraController(CameraController* ctl) { cameraCtl_ = ctl; }

    void shutdown();

    // 渲染（在 ImGui 窗口内调用：resize FBO → prepare → execute → Image）
    // sceneDirtyHint：多视图共享同一场景时，先渲染的视图会在 prepare 里清掉场景
    // 脏位；其它视图必须在整帧开始时抓到同一份快照并通过本参数显式传入，否则
    // 它们永远看不到本帧的变更（命令流不重建 → 副视图停在旧画面）。
    // 单视图 / 首个视图传 0（默认）→ 行为与原先完全一致。
    void render(const ImVec2& size, uint32_t sceneDirtyHint = 0);
    // 时间轴当前帧：传给 Renderer 作为表面预取缓存键（不影响渲染结果）
    void setTrajectoryFrame(uint64_t frame) { trajectoryFrame_ = frame; }

    // 相机交互（主方案 §7.10：命中视口才转发；左=0 中=1 右=2）
    bool handleMouse(float dx, float dy, int button);
    void handleScroll(float scrollY);

    // getDisplayDevice（主方案 §7.1：相机/材质/光照/裁剪面读写唯一通道）
    engine::OrbitCamera* getCamera() { return camera_; }

    // 当前场景（工具面板读取背景/雾效等场景属性用；实现在 cpp，Renderer 完整类型）
    [[nodiscard]] const entity::SceneStructure* scene() const;

    // 逻辑显示尺寸（导出"视口倍数"用）
    [[nodiscard]] uint32_t displayWidth() const { return displayW_; }
    [[nodiscard]] uint32_t displayHeight() const { return displayH_; }

    // 导出：离屏渲染到指定分辨率 → 回读 → PNG 写出。
    //   bgOverride   = nullptr 用场景背景；否则用给定 RGB（"白色背景"用）
    //   ssaa         = 超采样倍数（1 无；≥2 先渲染到 width*ssaa 再盒式降采样）
    //   colorDepth   = 8 或 16（16 时在浮点上求平均再量化，保留超采样的额外精度）
    //   transparentBg= true 时背景 alpha=0（PNG 可作合成素材；几何仍不透明）
    bool exportToPng(uint32_t width, uint32_t height, const std::string& path,
                     const float bgOverride[3] = nullptr, int ssaa = 1,
                     int colorDepth = 8, bool transparentBg = false);

    // 导出：自研 CPU 光线追踪（一.7）→ PNG；samplesPerPixel = 每像素子采样
    bool exportRayTracedToPng(uint32_t width, uint32_t height, const std::string& path,
                              const float bgOverride[3] = nullptr,
                              int samplesPerPixel = 2);

    // 导出：按轨迹帧逐帧写出 PNG 序列（动画导出，主方案 §12.6）。
    //   dirPath  = 输出目录（自动创建），文件名 frame_0000.png / frame_0001.png …
    //   instanceIndex / inFrame / outFrame / step 决定帧范围
    //   frameCountOut 返回实际写出的帧数
    // 注意：**必须在主线程调用**——逐帧改写场景坐标段（applyTrajectoryFrame）并
    // 重建命令流，与主线程的渲染循环共用同一场景/渲染器，跨线程会数据竞争。
    //   source: 0=轨迹；1=纯相机绕当前视轴转一周；2=轨迹+相机
    //   orbitFrames: source 含相机时转一周的帧数（0 → 120）
    bool exportAnimationToPng(uint32_t width, uint32_t height,
                              const std::string& dirPath, uint32_t instanceIndex,
                              uint64_t inFrame, uint64_t outFrame, uint32_t step,
                              const float bgOverride[3], int ssaa, int colorDepth,
                              uint32_t* frameCountOut, int source = 0,
                              uint32_t orbitFrames = 120);

    // 视口 RT 预览开关（一.7）：开启后视口用 CPU 光线追踪低分辨率渲染
    void setRayTracePreview(bool on) { rayTracePreview_ = on; markDirty(); }
    [[nodiscard]] bool rayTracePreview() const { return rayTracePreview_; }
    // 当前 Vulkan 设备是否暴露硬件光追扩展（不代表 CPU 光追导出可用性）
    [[nodiscard]] bool rayTraceSupported() const;

    // 分子显示视图叠加层：坐标轴方向 / 比例尺与场景尺寸。可由「显示」面板或
    // 视口「叠加」按钮分别关闭（用户要求"可选择关闭"）。
    void setViewOverlay(bool axes, bool scale) {
        if (showAxes_ != axes || showScaleBar_ != scale) {
            showAxes_ = axes;
            showScaleBar_ = scale;
            markDirty();
        }
    }

    // 动态分辨率：渲染尺度（0.35–2.0）。交互/播放时可按 dynamicResolution
    // 自动再乘一个内部系数（0.7），停手后回到设定值，从而提升流畅度。
    void setRenderScale(float s);
    [[nodiscard]] float renderScale() const { return renderScale_; }
    void setDynamicResolution(bool on) { dynamicResolution_ = on; markDirty(); }
    [[nodiscard]] bool dynamicResolution() const { return dynamicResolution_; }

    // 全局网格面数（球细分 1..4、圆柱截面 8..64）。修改后重建共享网格。
    void setMeshQuality(int detail, int segments);
    [[nodiscard]] int meshDetail() const { return meshDetail_; }
    [[nodiscard]] int meshSegments() const { return meshSegments_; }

    // 视口 FBO 纹理（ImGui Image 用）
    [[nodiscard]] ImTextureID textureId() const;

    // 释放已注册的 ImGui 纹理描述符（视口纹理 + RT 预览纹理）。
    // **必须在 ImGui 后端 Shutdown 之前调用**：描述符集由 ImGui 描述符池分配，
    // 池在后端 Shutdown 时被销毁，之后释放会操作已销毁的池。
    // 其余调用点（resize 重建、RT 纹理重建）在帧内执行，后端必然存活。
    void releaseImGuiTextures();

    // 重渲染标志
    void markDirty() { dirty_ = true; }
    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] bool isDragging() const { return isDragging_; }

private:
    // 释放单个 ImGui 纹理描述符并置零（空值安全）。
    // ImGui_ImplVulkan_AddTexture 每次都从描述符池分配一个描述符集，
    // 不释放会在每次 FBO 重建（窗口 resize 期间每帧一次）时泄漏一个；
    // 池上限 1000，持续拖拽窗口约 17 秒即耗尽。
    static void releaseDescriptor(ImTextureID& tex);

    // 相机状态指纹（方案 2b）：OrbitCamera 的 yaw/pitch/distance/fov/ortho/roll/
    // target/orthoScale 均为公开字段，外部直接赋值。此处不引入 setter，改为把
    // "可观测状态"压成单个 64 位指纹（按浮点位模式参与哈希，故 -0.0 等边界变化
    // 也能捕获），替代此前逐个镜像 7 个成员并逐字段比较的写法。
    [[nodiscard]] static uint64_t cameraFingerprint(const engine::OrbitCamera& cam);

    void renderRayTracePreview(const ImVec2& size);
    // 视口叠加：左下坐标轴方向 + 右下比例尺 + 右上场景尺寸（Å）
    void drawViewOverlay(const ImVec2& imgMin, const ImVec2& imgSize);
    // CPU → GPU 纹理上传（线性 tiling + host-visible + GENERAL 布局，免命令缓冲逐帧拷贝）
    bool ensureRtTexture(uint32_t w, uint32_t h);
    void updateRtTexture(const std::vector<uint8_t>& rgba);
    void destroyRtTexture();
    engine::Renderer* renderer_ = nullptr;
    engine::OrbitCamera* camera_ = nullptr;
    CameraController* cameraCtl_ = nullptr;   // 相机变更唯一入口（init 注入）
    std::unique_ptr<engine::OffscreenRenderer> offscreen_;

    // 导出目标（常驻复用，按需 resize）。此前每次导出都新建一个 OffscreenRenderer
    // → 重编 10 条管线 + 重建共享网格，约几十至上百毫秒的纯浪费。
    // 导出可能来自后台线程，故用互斥串行化（GPU 队列本身另由 device 锁串行）。
    std::mutex exportMutex_;
    std::unique_ptr<engine::OffscreenRenderer> exportTarget_;
    uint32_t viewportW_ = 0, viewportH_ = 0;   // 离屏渲染目标物理分辨率
    uint32_t displayW_ = 0, displayH_ = 0;     // 逻辑显示尺寸（拖拽换算 NDC）
    uint64_t trajectoryFrame_ = 0;             // 时间轴当前帧（表面预取缓存键）
    uint64_t lastSerial_ = UINT64_MAX;
    uint64_t lastCameraFp_ = 0;   // 上次渲染的相机指纹（替代逐个镜像 7 个字段）

    bool dirty_ = true;
    bool isDragging_ = false;
    int dragButton_ = -1;
    ImTextureID texture_ = ImTextureID(0);
    bool showAxes_ = true;
    bool showScaleBar_ = true;
    float renderScale_ = 1.0f;          // 用户设定的渲染分辨率比例
    bool dynamicResolution_ = true;      // 交互/播放时自动降分辨率
    int meshDetail_ = 3;                // 球细分档（1=80 面 … 4=5120 面）
    int meshSegments_ = 24;             // 圆柱截面段数

    // 播放/拖时间轴时的动态细节：坐标版本变化后 350 ms 内用低细分球/柱并跳过
    // 阴影 pass —— 轨迹播放每帧要重画 6000+ 个原子，高细分网格下会明显掉帧。
    uint64_t lastCoordsVersion_ = UINT64_MAX;
    std::chrono::steady_clock::time_point dynamicDetailUntil_{};
    bool lastDynamicDetail_ = false;
    // 场景尺寸缓存（按坐标版本失效；避免大体系每帧遍历全部坐标）
    uint64_t dimsVersion_ = UINT64_MAX;
    glm::vec3 dimsMin_{0.0f};
    glm::vec3 dimsMax_{0.0f};

    // —— CPU 光线追踪预览 ——
    bool rayTracePreview_ = false;
    VkImage rtImage_ = VK_NULL_HANDLE;
    VkDeviceMemory rtImageMem_ = VK_NULL_HANDLE;
    VkImageView rtView_ = VK_NULL_HANDLE;
    VkDeviceSize rtRowPitch_ = 0;
    void* rtMapped_ = nullptr;
    bool rtCoherent_ = true;
    uint32_t rtW_ = 0, rtH_ = 0;
    ImTextureID rtTexture_ = ImTextureID(0);

    // —— RT 级 LOD 状态 ——
    // 场景侧（RtScene + BVH）按命令流序号缓存：相机移动时只重投射，不重建。
    // 分辨率按「是否静止」两档：交互中用低分辨率跟手，停手 250 ms 后升级一份
    // 高清图（spp 也提到 2），下次交互再退回低档。
    engine::RtScene rtSceneCache_;
    std::shared_ptr<engine::RtAccel> rtAccelCache_;
    uint64_t rtCacheSerial_ = UINT64_MAX;
    bool rtCacheValid_ = false;
    double rtLastChangeTime_ = 0.0;   // 上次相机/场景变化时刻（ImGui::GetTime）
    bool rtRefined_ = false;          // 当前纹理是否已是「静止高清」档
};

} // namespace moleculens::app
