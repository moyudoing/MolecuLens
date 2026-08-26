// =============================================================================
// MolecuLens · engine · style/spacefill/spacefill.cpp —— 空间填充实现
// 规格来源：主方案 §5.7.1
//   for a in selection.mask():
//     r = vdwRadius[a.element] * rep.radiusScale
//     emit SphereInst { posRadius=(a.pos, r), color=colorOf(a), pickId=globalAtomId(a) }
// =============================================================================

#include "engine/style/spacefill/spacefill.h"

#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawSpacefill(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        SphereInst inst;
        inst.posRadius = glm::vec4(pos[g], elementInfo(st.elements[i]).vdwRadius *
                                                ctx.rep->radiusScale);
        inst.color = Representer::colorForAtom(ctx, g);
        inst.pickId = glm::uvec2(g, 0);
        inst.pad = glm::uvec2(0, 0);
        out.append(Cmd::SphereInst, &inst, sizeof(inst));
    }
}

// VDW：范德华全半径空间填充——半径用元素 vdW 半径本身，不受"球体缩放"
// （radiusScale）影响，与 CPK（Spacefill，vdW × radiusScale）区别开。
void drawVdw(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        SphereInst inst;
        // 全半径：元素 vdW 半径，忽略 ctx.rep->radiusScale
        inst.posRadius = glm::vec4(pos[g], elementInfo(st.elements[i]).vdwRadius);
        inst.color = Representer::colorForAtom(ctx, g);
        inst.pickId = glm::uvec2(g, 0);
        inst.pad = glm::uvec2(0, 0);
        out.append(Cmd::SphereInst, &inst, sizeof(inst));
    }
}

} // namespace moleculens::engine
