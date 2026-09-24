// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.h —— 3D 视口封装
// 规格来源：主方案 §7.7（离屏 FBO → ImGui::Image；相机交互：左键轨道/中键缩放/
//           右键平移/滚轮 dolly；getDisplayDevice 渲染参数出口）
// =============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include <glm/glm.hpp>
#include <imgui.h>
#include <volk.h>

#include "spdlog/spdlog.h"

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
    void render(const ImVec2& size);

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

    // 视口 RT 预览开关（一.7）：开启后视口用 CPU 光线追踪低分辨率渲染
    void setRayTracePreview(bool on) { rayTracePreview_ = on; markDirty(); }
    [[nodiscard]] bool rayTracePreview() const { return rayTracePreview_; }

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
    uint64_t lastSerial_ = UINT64_MAX;
    uint64_t lastCameraFp_ = 0;   // 上次渲染的相机指纹（替代逐个镜像 7 个字段）

    bool dirty_ = true;
    bool isDragging_ = false;
    int dragButton_ = -1;
    ImTextureID texture_ = ImTextureID(0);

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
};

} // namespace moleculens::app
