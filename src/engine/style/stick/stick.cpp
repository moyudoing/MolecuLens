// =============================================================================
// MolecuLens · engine · style/stick/stick.cpp —— 棍状实现
// 规格来源：主方案 §5.7.3
// =============================================================================

#include "engine/style/stick/stick.h"

#include "engine/render/render_parameter.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawStick(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    const float cylR = 0.25f * ctx.rep->radiusScale;

    // 键圆柱
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
        glm::vec3 s, e;
        // 棍状两端无需内缩（原子端帽可隐）；简单做法：端面缩 0.25×cylR 防穿
        const glm::vec3 dir = glm::normalize(pos[gb] - pos[ga]);
        s = pos[ga] + dir * cylR * 0.5f;
        e = pos[gb] - dir * cylR * 0.5f;

        CylinderInst inst;
        inst.aPos = glm::vec4(s, 0.0f);
        inst.bPos = glm::vec4(e, 0.0f);
        inst.radius = cylR;
        inst.pad = 0.0f;
        const glm::vec4 ca = Representer::colorForAtom(ctx, ga);
        const glm::vec4 cb = Representer::colorForAtom(ctx, gb);
        inst.color = (ca + cb) * 0.5f;
        inst.pickId_pad = glm::uvec2(std::min(ga, gb), 0);
        out.append(Cmd::CylinderInst, &inst, sizeof(inst));
    }

    // 可选：原子端帽小圆点（防键端突兀；主方案 §5.7.3 可选）
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        SphereInst inst;
        inst.posRadius = glm::vec4(pos[g], cylR * 0.8f);
        inst.color = Representer::colorForAtom(ctx, g);
        inst.pickId = glm::uvec2(g, 0);
        inst.pad = glm::uvec2(0, 0);
        out.append(Cmd::SphereInst, &inst, sizeof(inst));
    }
}

} // namespace moleculens::engine
