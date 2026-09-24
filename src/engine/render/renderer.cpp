// =============================================================================
// MolecuLens · engine · render/renderer.cpp —— Renderer 实现
// 规格来源：主方案 §5.4 / §5.8.2（脏位域 → 收集任务 → 并行生成 → 保序合并 →
//           serial++；帧切只重建坐标段由 VulkanRenderer 处理）
// =============================================================================

#include "engine/render/renderer.h"

#include <algorithm>

#include "engine/rep/representer.h"
#include "engine/style/quicksurf/quicksurf.h"
#include "engine/style/surface/surface.h"
#include "engine/vulkan/realtime/vulkan_renderer.h"
#include "entity/element/element.h"
#include "entity/scene/scene.h"
#include "entity/selection/selection.h"

namespace moleculens::engine {

Renderer::~Renderer() = default;

void Renderer::bindRepresentations(const std::vector<Representation>* reps,
                                   uint64_t revision) {
    if (representations_ != reps || repsRevision_ != revision) {
        representations_ = reps;
        repsRevision_ = revision;
        needsRebuild_ = true;
    }
}

void Renderer::unbindRepresentations() {
    representations_ = nullptr;
    repsRevision_ = 0;
}

const std::vector<Representation>& Renderer::representations() const {
    static const std::vector<Representation> kNone;
    return representations_ ? *representations_ : kNone;
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
    // 每 (表示, 实例) 生成段（P10 并行；此处串行实现，M2 接入 TaskPool）——
    // Selection 掩码对整个场景求值一次（与实例无关），本表示全部实例复用。
    for (const auto& rep : representations()) {
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

        // 体系级表面（G5）：整体系（含溶剂分子）一个整体表面，只发射一次（而非
        // 每分子一个独立小表面）。surface/quicksurf 表示设置 rep.systemSurface 时生效。
        if (rep.systemSurface &&
            (rep.type == RepresentationType::Surface ||
             rep.type == RepresentationType::QuickSurf)) {
            // 整体表面是单一 TriMesh，恒以体系渲染模式「实心」(mode0) 绘制；
            // 必须先重置 RenderMode 与材质：若此前其它表示的最后一条 RenderMode/
            // MaterialBlock 命令残留，表面网格会继承错误的管线（线框/透明）或全零
            // 默认材质（diffuse=0）→ 烘焙后顶点色全黑 → 表面不可见。
            RenderModeInst rmi0;
            rmi0.mode = 0;
            rmi0.pad[0] = rmi0.pad[1] = rmi0.pad[2] = 0;
            fresh.append(Cmd::RenderMode, &rmi0, sizeof(rmi0));
            MaterialBlock mat0;
            mat0.ambient = glm::vec4(rep.ambient[0], rep.ambient[1], rep.ambient[2],
                                     rep.ambient[3]);
            mat0.diffuse = glm::vec4(rep.diffuse[0], rep.diffuse[1], rep.diffuse[2],
                                     rep.diffuse[3]);
            mat0.specular = glm::vec4(rep.specular[0], rep.specular[1], rep.specular[2],
                                      rep.specular[3]);
            mat0.shininess = rep.shininess;
            mat0.metallic = rep.metallic;
            mat0.roughness = rep.roughness;
            mat0.opacity = rep.opacity;
            fresh.append(Cmd::MaterialBlock, &mat0, sizeof(mat0));

            RepContext sctx;
            sctx.scene = scene_;
            sctx.frame = &frameData_;
            sctx.rep = &rep;
            sctx.mask = &mask;
            if (rep.type == RepresentationType::Surface) {
                drawSystemSurface(sctx, fresh);
            } else {
                drawSystemQuickSurf(sctx, fresh);
            }
            continue;   // 跳过逐实例生成（整体系一个表面已足够）
        }

        // 下标遍历（实例下标即所需参数；此前按引用做线性反查 → 每实例一次
        // O(n) 扫描，整体 O(n²)，大体系下是 prepare 的主要开销之一）
        const auto& instances = scene_->instances();
        for (uint32_t instIdx = 0; instIdx < instances.size(); ++instIdx) {
            const auto& inst = instances[instIdx];
            if (!inst.visible) {
                continue;
            }
            // 体系显隐：整个体系隐藏则其成员整体不渲染（体系作为整体对象）
            if (inst.systemId != UINT32_MAX) {
                const uint32_t si = scene_->systemIndexById(inst.systemId);
                if (si != UINT32_MAX && !scene_->systems()[si].visible) {
                    continue;
                }
            }

            Representer representer;
            representer.generate(rep, *scene_, inst, instIdx, frameData_, 0,
                                 mask, fresh);
        }
    }

    commandList_ = std::move(fresh);
    commandList_.bumpSerial();   // serial 递增：视图层用其检测命令流重建（缓存键 P5）
    scene_->clearDirty();
}

void Renderer::draw(const RenderView& view) {
    if (vulkan_ == nullptr) {
        return;
    }
    vulkan_->execute(commandList_, view);
}

LightParams Renderer::sceneLighting() const {
    LightParams light;
    if (!scene_) {
        return light;
    }
    // 全局光：天顶/地面半球色 + 均匀环境光（无方向光/点光源）
    const auto& props = scene_->properties();
    light.skyColor = glm::vec3(props.skyColor[0], props.skyColor[1], props.skyColor[2]);
    light.groundColor =
        glm::vec3(props.groundColor[0], props.groundColor[1], props.groundColor[2]);
    light.ambientColor =
        glm::vec3(props.ambientColor[0], props.ambientColor[1], props.ambientColor[2]);
    light.ambientIntensity = props.ambient;
    return light;
}

glm::vec4 Renderer::sceneFog() const {
    if (!scene_) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 0.05f);   // 与 RenderView 默认一致（雾关）
    }
    const auto& props = scene_->properties();
    return glm::vec4(props.fog[0], props.fog[1], props.fogMode, props.fogDensity);
}

RenderView Renderer::viewFor(const OrbitCamera& camera, uint32_t width,
                             uint32_t height) const {
    RenderView view;
    view.camera = camera;
    view.width = std::max(1u, width);
    view.height = std::max(1u, height);
    view.light = sceneLighting();
    view.fog = sceneFog();
    return view;
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
    float radius = glm::length(hi - lo) * 0.5f;
    // 半径下限 = 所有原子 vdW 半径的最大值（单原子/全部重叠时包围球退化为
    // 点，radius=0 → centerOn 距离/近远面退化 → 相机贴在原子内 → 不可见）。
    // 取最大 vdW 半径保证相机距离合理，单原子在各渲染模式（VDW/表面/球棍…）
    // 都能正确显示。
    float minRadius = 1.0f;
    for (const auto& ms : scene_->structures()) {
        for (const auto el : ms.elements) {
            minRadius = std::max(minRadius, entity::elementInfo(el).vdwRadius);
        }
    }
    *outRadius = std::max(radius, minRadius);
    return true;
}

} // namespace moleculens::engine
