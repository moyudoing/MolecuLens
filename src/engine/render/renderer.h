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

    [[nodiscard]] const SceneCommandList& commandList() const { return commandList_; }
    SceneCommandList& commandList() { return commandList_; }
    [[nodiscard]] uint64_t commandSerial() const { return commandList_.serial(); }

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

    // 一次渲染所需的完整视图参数（相机 + 视口尺寸 + 光照/雾）。
    [[nodiscard]] RenderView viewFor(const OrbitCamera& camera, uint32_t width,
                                     uint32_t height) const;

private:
    void rebuildCommandList();            // 串行/并行生成命令流

    entity::SceneStructure* scene_ = nullptr;
    VulkanRenderer* vulkan_ = nullptr;
    SceneCommandList commandList_;
    entity::FrameData frameData_;
    const std::vector<Representation>* representations_ = nullptr;   // 非拥有
    uint64_t repsRevision_ = 0;      // 绑定时的表示修订号（变更即触发重建）
    bool needsRebuild_ = true;
};

} // namespace moleculens::engine
