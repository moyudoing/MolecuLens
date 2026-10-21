// =============================================================================
// MolecuLens · engine · style/dotcloud/dotcloud.cpp —— 点云（Point sprite）实现
//
// 为什么单独做一个样式：体系渲染模式「点」是全局开关（整个体系都变成点），
// 而用户经常需要"同一体系里球棍 + 一层点云高亮"或"超大体系只画点"。样式内部
// 临时发一条 RenderMode 命令、渲染完再恢复，即可按**表示**粒度使用点模式。
//
// 坐标重放（轨迹播放）：RenderMode 是状态命令，不在重放计划里；ctx.replay 为
// true 时跳过状态命令（旧命令流里已经有了），只重写 SphereInst 坐标载荷。
// =============================================================================

#include "engine/style/dotcloud/dotcloud.h"

#include <algorithm>
#include <cstdint>

#include <glm/glm.hpp>

#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawDotCloud(const RepContext& ctx, SceneCommandList& out) {
    if (ctx.scene == nullptr || ctx.instance == nullptr || ctx.mask == nullptr ||
        ctx.rep == nullptr || ctx.instance->structureIndex >= ctx.scene->structures().size()) {
        return;
    }
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (pos == nullptr) {
        return;
    }

    // 点模式（0 实心 / 1 线框 / 2 透明 / 3 点）。重放路径不重发状态命令。
    if (!ctx.replay) {
        RenderModeInst pointMode;
        pointMode.mode = 3;
        pointMode.pad[0] = pointMode.pad[1] = pointMode.pad[2] = 0;
        out.append(Cmd::RenderMode, &pointMode, sizeof(pointMode));
    }

    const float radius = std::max(0.05f, 0.55f * ctx.rep->radiusScale);
    constexpr size_t kBatch = 4096;
    std::vector<SphereInst> batch;
    batch.reserve(std::min(kBatch, st.atoms.size()));
    auto flush = [&]() {
        if (!batch.empty()) {
            out.append(Cmd::SphereInst, batch.data(),
                       static_cast<uint32_t>(batch.size() * sizeof(SphereInst)));
            batch.clear();
        }
    };
    for (size_t i = 0; i < st.atoms.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        SphereInst inst;
        inst.posRadius = glm::vec4(pos[g], radius);
        inst.color = Representer::colorForAtom(ctx, g);
        inst.pickId = glm::uvec2(g, 0);
        inst.pad = glm::uvec2(0, 0);
        batch.push_back(inst);
        if (batch.size() >= kBatch) {
            flush();
        }
    }
    flush();

    if (!ctx.replay) {
        RenderModeInst restore;
        restore.mode = ctx.renderMode;
        restore.pad[0] = restore.pad[1] = restore.pad[2] = 0;
        out.append(Cmd::RenderMode, &restore, sizeof(restore));
    }
}

} // namespace moleculens::engine
