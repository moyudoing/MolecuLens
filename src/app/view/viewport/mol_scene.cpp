// =============================================================================
// MolecuLens · app · view/viewport/mol_scene.cpp —— 3D 视口实现
// 规格来源：主方案 §7.7（FBO 方案：场景渲染进离屏 FBO，纹理直接给 ImGui 采样）
// =============================================================================

#include "app/view/viewport/mol_scene.h"

#include <imgui_impl_vulkan.h>

#include "engine/camera/camera.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/scene/scene.h"

namespace moleculens::app {

MolScene::MolScene() = default;

MolScene::~MolScene() { shutdown(); }

void MolScene::init(engine::Renderer* renderer, engine::OrbitCamera* camera) {
    renderer_ = renderer;
    camera_ = camera;
    offscreen_ = std::make_unique<engine::OffscreenRenderer>();
    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    std::string err;
    if (!offscreen_->init(cfg, 640, 480, &err)) {
        spdlog::error("[ui] offscreen viewport init failed: {}", err);
        offscreen_.reset();
    }
}

void MolScene::shutdown() {
    offscreen_.reset();
    texture_ = ImTextureID(0);
}

void MolScene::render(const ImVec2& size) {
    if (!renderer_ || !camera_ || !offscreen_ || !renderer_->scene()) {
        ImGui::Text("3D 视口未初始化（先打开文件）");
        return;
    }
    const uint32_t w = static_cast<uint32_t>(std::max(1.0f, size.x));
    const uint32_t h = static_cast<uint32_t>(std::max(1.0f, size.y));
    if (w != viewportW_ || h != viewportH_) {
        offscreen_->resize(w, h);
        viewportW_ = w;
        viewportH_ = h;
        dirty_ = true;
        texture_ = ImTextureID(0);   // FBO 重建 → 纹理重建
    }
    // 相机移动 / 场景脏 → 重渲染
    const bool cameraMoved = (camera_->yaw != lastYaw_ || camera_->pitch != lastPitch_ ||
                              camera_->distance != lastDist_ ||
                              camera_->target != lastTarget_);
    if (dirty_ || cameraMoved || renderer_->commandSerial() != lastSerial_) {
        const auto& props = renderer_->scene()->properties();
        offscreen_->setBackground(props.bgColor);
        renderer_->prepare();
        engine::RenderView view;
        view.camera = *camera_;
        view.width = w;
        view.height = h;
        offscreen_->execute(renderer_->commandList(), view);
        lastSerial_ = renderer_->commandSerial();
        lastYaw_ = camera_->yaw;
        lastPitch_ = camera_->pitch;
        lastDist_ = camera_->distance;
        lastTarget_ = camera_->target;
        dirty_ = false;
    }
    if (texture_ == ImTextureID(0) && offscreen_->colorView() != VK_NULL_HANDLE) {
        // vcpkg imgui 的 AddTexture 返回 VkDescriptorSet（旧版签名）→ 转 ImTextureID
        const VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
            offscreen_->sampler(), offscreen_->colorView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        texture_ = ImTextureID(reinterpret_cast<uintptr_t>(desc));
    }
    if (texture_) {
        ImGui::Image(texture_, size);
    } else {
        ImGui::Text("视口纹理不可用");
    }
}

bool MolScene::handleMouse(float dx, float dy, int button) {
    if (!camera_) {
        return false;
    }
    if (ImGui::IsMouseDown(button) && !isDragging_) {
        isDragging_ = true;
        dragButton_ = button;
    }
    if (!isDragging_ || dragButton_ != button) {
        return false;
    }
    // 主方案 §7.10：左键轨道旋转 / 中键缩放 / 右键平移
    const float sensitivity = 0.008f;
    if (button == 0) {
        camera_->orbit(dx * sensitivity, dy * sensitivity);
    } else if (button == 1) {
        camera_->dolly(1.0f + dy * sensitivity);
    } else if (button == 2) {
        const float w = static_cast<float>(std::max(1u, viewportW_));
        const float h = static_cast<float>(std::max(1u, viewportH_));
        camera_->pan(glm::vec2(-dx / w * 2.0f, dy / h * 2.0f));
    }
    if (!ImGui::IsMouseDown(button)) {
        isDragging_ = false;
        dragButton_ = -1;
    }
    return true;
}

void MolScene::handleScroll(float scrollY) {
    if (camera_) {
        // 滚轮缩放（主方案 §7.10：1 - scroll*0.15）
        camera_->dolly(1.0f - scrollY * 0.15f);
    }
}

void MolScene::handleClick() {
    // M2：拾取（ID Pass / CPU 射线）
}

ImTextureID MolScene::textureId() const {
    return texture_;
}

} // namespace moleculens::app
