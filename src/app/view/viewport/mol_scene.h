// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.h —— 3D 视口封装
// 规格来源：主方案 §7.7（离屏 FBO → ImGui::Image；相机交互：左键轨道/中键缩放/
//           右键平移/滚轮 dolly；getDisplayDevice 渲染参数出口）
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>
#include <imgui.h>

#include "spdlog/spdlog.h"

namespace moleculens::engine {
class Renderer;
class OrbitCamera;
class OffscreenRenderer;
} // namespace moleculens::engine

namespace moleculens::app {

class MolScene {
public:
    MolScene();      // 定义于 cpp（unique_ptr<OffscreenRenderer> 成员需完整类型）
    ~MolScene();

    // 初始化（命令流共享；离屏渲染器独占本类）
    void init(engine::Renderer* renderer, engine::OrbitCamera* camera);
    void shutdown();

    // 渲染（在 ImGui 窗口内调用：resize FBO → prepare → execute → Image）
    void render(const ImVec2& size);

    // 相机交互（主方案 §7.10：命中视口才转发；左=0 中=1 右=2）
    bool handleMouse(float dx, float dy, int button);
    void handleScroll(float scrollY);
    void handleClick();

    // getDisplayDevice（主方案 §7.1：相机/材质/光照/裁剪面读写唯一通道）
    engine::OrbitCamera* getCamera() { return camera_; }

    // 视口 FBO 纹理（ImGui Image 用）
    ImTextureID textureId() const;

    // 重渲染标志
    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }
    bool isDragging() const { return isDragging_; }

private:
    engine::Renderer* renderer_ = nullptr;
    engine::OrbitCamera* camera_ = nullptr;
    std::unique_ptr<engine::OffscreenRenderer> offscreen_;
    uint32_t viewportW_ = 0, viewportH_ = 0;
    uint64_t lastSerial_ = UINT64_MAX;
    float lastYaw_ = 0.0f, lastPitch_ = 0.5f, lastDist_ = 40.0f;
    glm::vec3 lastTarget_{0.0f};
    bool dirty_ = true;
    bool isDragging_ = false;
    int dragButton_ = -1;
    ImTextureID texture_ = ImTextureID(0);
};

} // namespace moleculens::app
