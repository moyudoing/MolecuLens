// =============================================================================
// MolecuLens · engine · style/ballstick/ballstick.cpp —— 球棍实现
// 规格来源：主方案 §5.7.2
// =============================================================================

#include "engine/style/ballstick/ballstick.h"

#include <algorithm>

#include "engine/render/render_parameter.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawBallAndStick(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    // 原子球半径预计算
    std::vector<float> sphereR(n);
    std::vector<bool> atomSelected(n, false);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        atomSelected[i] = true;
        // r = clamp(0.25 * vdwRadius, 0.2, 0.6) * radiusScale（主方案 §5.7.2）
        sphereR[i] = std::clamp(0.25f * elementInfo(st.elements[i]).vdwRadius,
                                0.2f, 0.6f) * ctx.rep->radiusScale;
    }

    // Pass A 原子球
    for (size_t i = 0; i < n; ++i) {
        if (!atomSelected[i]) {
            continue;
        }
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        SphereInst inst;
        inst.posRadius = glm::vec4(pos[g], sphereR[i]);
        inst.color = Representer::colorForAtom(ctx, g);
        inst.pickId = glm::uvec2(g, 0);
        inst.pad = glm::uvec2(0, 0);
        out.append(Cmd::SphereInst, &inst, sizeof(inst));
    }

    // Pass B 键（两端均命中的键）
    const float cylR = 0.15f * ctx.rep->radiusScale * ctx.rep->bondScale;
    for (const entity::Bond& bnd : st.bonds) {
        if (bnd.a >= n || bnd.b >= n ||
            !atomSelected[bnd.a] || !atomSelected[bnd.b]) {
            continue;
        }
        // 安全网：PBC 补齐在环状分子/三斜盒上仍可能留下一条跨盒长键。把明显
        // 超出共价键范围（> 3.2 Å）的键直接跳过，避免视口出现贯穿整个盒子的
        // "长棍"；原子本身仍按补齐后的坐标绘制。
        const glm::vec3& pa = pos[ctx.globalAtomBase + bnd.a];
        const glm::vec3& pb = pos[ctx.globalAtomBase + bnd.b];
        const glm::vec3 bd = pb - pa;
        if (glm::dot(bd, bd) > 3.2f * 3.2f) {
            continue;
        }
        glm::vec3 s, e;
        BondRenderParams::computeEndpoints(pos[ctx.globalAtomBase + bnd.a],
                                           pos[ctx.globalAtomBase + bnd.b],
                                           sphereR[bnd.a], sphereR[bnd.b],
                                           cylR, &s, &e);
        const uint32_t ga = ctx.globalAtomBase + bnd.a;
        const uint32_t gb = ctx.globalAtomBase + bnd.b;
        CylinderInst inst;
        inst.aPos = glm::vec4(s, 0.0f);
        inst.bPos = glm::vec4(e, 0.0f);
        inst.radius = cylR;
        inst.pad = 0.0f;
        // 键色 = 两端原子色平均（主方案 §5.7.2）
        const glm::vec4 ca = Representer::colorForAtom(ctx, ga);
        const glm::vec4 cb = Representer::colorForAtom(ctx, gb);
        inst.color = (ca + cb) * 0.5f;
        inst.pickId_pad = glm::uvec2(std::min(ga, gb), 0);
        out.append(Cmd::CylinderInst, &inst, sizeof(inst));
    }
}

} // namespace moleculens::engine
