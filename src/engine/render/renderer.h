// =============================================================================
// MolecuLens · engine · render/renderer.h —— Renderer（prepare/draw 编排）
// 规格来源：主方案 §5.4 / §5.8（Stage1 并行生成命令流；Stage2 后端执行；
//           脏位域驱动；serial 缓存键 P5）
// =============================================================================

#pragma once

#include <memory>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/picking/picking.h"
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
    glm::vec4 fog{0.0f, 0.0f, 0.0f, 0.05f};     // 雾：x near, y far, z mode(0关), w density
};

class Renderer {
public:
    ~Renderer();

    // 场景指针（Renderer 负责置脏/清脏，主方案 §5.4 prepare 阶段）
    void setScene(entity::SceneStructure* scene) { scene_ = scene; }
    [[nodiscard]] const entity::SceneStructure* scene() const { return scene_; }
    void setVulkanRenderer(VulkanRenderer* vk) { vulkan_ = vk; }

    void update(float dt);
    void prepare();                       // ★ Stage1: 并行生成命令流（脏则重建）
    void draw(const RenderView& view);    // ★ Stage2: 命令流 → 后端执行
    void rebuildRenderData();             // 脏 → 重生成（serial 递增）

    PickResult pick(uint32_t x, uint32_t y);

    [[nodiscard]] const SceneCommandList& commandList() const { return commandList_; }
    SceneCommandList& commandList() { return commandList_; }
    [[nodiscard]] uint64_t commandSerial() const { return commandList_.serial(); }

    // 表示列表（场景级）
    std::vector<Representation>& representations() { return representations_; }
    [[nodiscard]] const std::vector<Representation>& representations() const { return representations_; }
    void setRepresentations(std::vector<Representation> reps);

    // 帧数据（当前帧坐标段；由外部写 CoordsSoA 后刷新）
    void refreshFrameData();

    [[nodiscard]] bool prepareParallel() const { return prepareParallel_; }
    void setPrepareParallel(bool on) { prepareParallel_ = on; }

    // 场景包围球（自动取景用）
    bool sceneBounds(glm::vec3* outCenter, float* outRadius) const;

    // 从场景属性填装 RenderView 光照（应用层构建 RenderView 后调用）
    void applySceneLighting(RenderView& view) const;

private:
    void rebuildCommandList();            // 串行/并行生成命令流

    entity::SceneStructure* scene_ = nullptr;
    VulkanRenderer* vulkan_ = nullptr;
    SceneCommandList commandList_;
    entity::FrameData frameData_;
    std::vector<Representation> representations_;
    bool prepareParallel_ = true;
    bool needsRebuild_ = true;
};

} // namespace moleculens::engine
