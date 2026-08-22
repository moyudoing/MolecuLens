// =============================================================================
// MolecuLens · engine · render/renderer.cpp —— Renderer 实现
// 规格来源：主方案 §5.4 / §5.8.2（脏位域 → 收集任务 → 并行生成 → 保序合并 →
//           serial++；帧切只重建坐标段由 VulkanRenderer 处理）
// =============================================================================

#include "engine/render/renderer.h"

#include <algorithm>

#include "engine/rep/representer.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "entity/scene/scene.h"
#include "entity/selection/selection.h"

namespace moleculens::engine {

namespace {

// 默认表示（首加载自动创建球棍，主方案 §5.2/§7.7）
std::vector<Representation> defaultRepresentations() {
    Representation rep;
    rep.name = "ballstick";
    rep.type = RepresentationType::BallAndStick;
    rep.selection = "";
    rep.colorScheme = ColorScheme::ElementCPK;
    return {rep};
}

} // namespace

Renderer::~Renderer() = default;

void Renderer::setRepresentations(std::vector<Representation> reps) {
    representations_ = std::move(reps);
    needsRebuild_ = true;
}

void Renderer::refreshFrameData() {
    // 帧数据：坐标段直接引用 CoordsSoA（世界坐标）
    if (scene_) {
        frameData_.positions = &scene_->coordsSoA().positions;
        frameData_.timestep = nullptr;
    }
}

void Renderer::update(float dt) {
    (void)dt;
    if (scene_ == nullptr) {
        return;
    }
    // 脏位域 → 需要重建
    if (scene_->dirtyBits() != 0) {
        needsRebuild_ = true;
    }
}

void Renderer::prepare() {
    if (scene_ == nullptr) {
        return;
    }
    if (!needsRebuild_) {
        return;
    }
    rebuildCommandList();
    needsRebuild_ = false;
}

void Renderer::rebuildCommandList() {
    refreshFrameData();

    SceneCommandList fresh;
    // 每 (实例, 表示) 一个生成段（P10 并行；此处串行实现，M2 接入 TaskPool）
    for (const auto& inst : scene_->instances()) {
        if (!inst.visible) {
            continue;
        }
        for (const auto& rep : representations_) {
            if (!rep.visible) {
                continue;
            }
            // Selection 求值（跨实例全局掩码）
            std::vector<bool> mask;
            if (rep.selection.empty()) {
                mask.assign(scene_->coordsSoA().atomIds.size(), true);
            } else {
                entity::Selection sel;
                const entity::ParseTable& table = entity::ParseTable::defaultTable();
                if (sel.parse(rep.selection, table)) {
                    sel.evaluate(*scene_, frameData_, 0);
                    mask = sel.mask();
                } else {
                    mask.assign(scene_->coordsSoA().atomIds.size(), true);
                }
            }

            // 实例下标查找
            uint32_t instIdx = 0;
            for (uint32_t i = 0; i < scene_->instances().size(); ++i) {
                if (&scene_->instances()[i] == &inst) {
                    instIdx = i;
                    break;
                }
            }

            Representer representer;
            representer.generate(rep, *scene_, inst, instIdx, frameData_, 0,
                                 mask, fresh);
        }
    }

    commandList_ = std::move(fresh);
    scene_->clearDirty();
}

void Renderer::draw(const RenderView& view) {
    if (vulkan_ == nullptr) {
        return;
    }
    vulkan_->execute(commandList_, view);
}

PickResult Renderer::pick(uint32_t x, uint32_t y) {
    (void)x; (void)y;
    return PickResult{};
}

bool Renderer::sceneBounds(glm::vec3* outCenter, float* outRadius) const {
    if (scene_ == nullptr || scene_->coordsSoA().positions.empty()) {
        return false;
    }
    const auto& pos = scene_->coordsSoA().positions;
    glm::vec3 lo = pos[0], hi = pos[0];
    for (const auto& p : pos) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    *outCenter = (lo + hi) * 0.5f;
    *outRadius = glm::length(hi - lo) * 0.5f;
    return true;
}

} // namespace moleculens::engine
