// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.h —— 3D 视口封装
// 规格来源：主方案 §7.7（离屏 FBO → ImGui::Image；相机交互：左键轨道/中键缩放/
//           右键平移/滚轮 dolly；getDisplayDevice 渲染参数出口）
// =============================================================================

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

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

class MolScene {
public:
    MolScene();      // 定义于 cpp（unique_ptr<OffscreenRenderer> 成员需完整类型）
    ~MolScene();

    // 初始化（命令流共享；离屏渲染器独占本类；vk = 主窗口 VulkanRenderer，
    // 离屏 FBO 建在其设备上 → ImGui 才能同设备采样纹理）
    void init(engine::Renderer* renderer, engine::OrbitCamera* camera,
              engine::VulkanRenderer* vk);
    void shutdown();

    // 渲染（在 ImGui 窗口内调用：resize FBO → prepare → execute → Image）
    void render(const ImVec2& size);

    // 相机交互（主方案 §7.10：命中视口才转发；左=0 中=1 右=2）
    bool handleMouse(float dx, float dy, int button);
    void handleScroll(float scrollY);
    void handleClick();

    // getDisplayDevice（主方案 §7.1：相机/材质/光照/裁剪面读写唯一通道）
    engine::OrbitCamera* getCamera() { return camera_; }

    // 当前场景（工具面板读取背景/雾效等场景属性用；实现在 cpp，Renderer 完整类型）
    const entity::SceneStructure* scene() const;

    // 逻辑显示尺寸（导出"视口倍数"用）
    [[nodiscard]] uint32_t displayWidth() const { return displayW_; }
    [[nodiscard]] uint32_t displayHeight() const { return displayH_; }

    // 导出：离屏渲染到指定分辨率 → RGBA8 回读 → PNG 写出（M4 落地）
    // bgOverride = nullptr 用场景背景；否则用给定 RGB（导出"白色背景"用）。
    // ssaa = 超采样倍数（1 无；≥2 先渲染到 width*ssaa 再盒式降采样 → 抗锯齿）。
    bool exportToPng(uint32_t width, uint32_t height, const std::string& path,
                     const float bgOverride[3] = nullptr, int ssaa = 1);

    // 导出：自研 CPU 光线追踪（一.7）→ PNG；samplesPerPixel = 每像素子采样
    bool exportRayTracedToPng(uint32_t width, uint32_t height, const std::string& path,
                              const float bgOverride[3] = nullptr,
                              int samplesPerPixel = 2);

    // 视口 RT 预览开关（一.7）：开启后视口用 CPU 光线追踪低分辨率渲染
    void setRayTracePreview(bool on) { rayTracePreview_ = on; markDirty(); }
    bool rayTracePreview() const { return rayTracePreview_; }

    // 视口 FBO 纹理（ImGui Image 用）
    [[nodiscard]] ImTextureID textureId() const;

    // 重渲染标志
    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }
    bool isDragging() const { return isDragging_; }

private:
    void renderRayTracePreview(const ImVec2& size);
    // CPU → GPU 纹理上传（线性 tiling + host-visible + GENERAL 布局，免命令缓冲逐帧拷贝）
    bool ensureRtTexture(uint32_t w, uint32_t h);
    void updateRtTexture(const std::vector<uint8_t>& rgba);
    void destroyRtTexture();
    engine::Renderer* renderer_ = nullptr;
    engine::OrbitCamera* camera_ = nullptr;
    std::unique_ptr<engine::OffscreenRenderer> offscreen_;
    uint32_t viewportW_ = 0, viewportH_ = 0;   // 离屏渲染目标物理分辨率
    uint32_t displayW_ = 0, displayH_ = 0;     // 逻辑显示尺寸（拖拽换算 NDC）
    uint64_t lastSerial_ = UINT64_MAX;
    float lastYaw_ = 0.0f, lastPitch_ = 0.5f, lastDist_ = 40.0f;
    float lastFov_ = 45.0f;
    float lastRoll_ = 0.0f;
    bool lastOrtho_ = false;
    glm::vec3 lastTarget_{0.0f};
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
