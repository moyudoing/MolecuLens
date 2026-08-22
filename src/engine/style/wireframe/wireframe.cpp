// =============================================================================
// MolecuLens · engine · style/wireframe/wireframe.cpp —— 线状实现
// 规格来源：主方案 §5.7.4
// =============================================================================

#include "engine/style/wireframe/wireframe.h"

#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawWireframe(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    for (const entity::Bond& bnd : st.bonds) {
        if (bnd.a >= n || bnd.b >= n) {
            continue;
        }
        const uint32_t ga = ctx.globalAtomBase + bnd.a;
        const uint32_t gb = ctx.globalAtomBase + bnd.b;
        if (ga >= ctx.mask->size() || gb >= ctx.mask->size() ||
            !(*ctx.mask)[ga] || !(*ctx.mask)[gb]) {
            continue;
        }
        LineInst inst;
        inst.aPos = glm::vec4(pos[ga], 0.0f);
        inst.bPos = glm::vec4(pos[gb], 0.0f);
        inst.color = Representer::colorForAtom(ctx, ga);
        out.append(Cmd::LineInst, &inst, sizeof(inst));
    }
}

} // namespace moleculens::engine
