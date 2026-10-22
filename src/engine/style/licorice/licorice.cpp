// =============================================================================
// MolecuLens · engine · style/licorice/licorice.cpp —— Licorice 实现
// =============================================================================

#include "engine/style/licorice/licorice.h"

#include <glm/glm.hpp>

#include "engine/rep/representer.h"

#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawLicorice(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    const size_t n = st.elements.size();

    // 甘草棍：粗圆柱（比 Stick 更粗），无端帽球
    const float cylR = 0.30f * ctx.rep->radiusScale;

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
        // PBC 残留长键保护：>3.2 Å 视为跨盒伪键，不画。
        {
            const glm::vec3 bd = pos[gb] - pos[ga];
            if (glm::dot(bd, bd) > 3.2f * 3.2f) {
                continue;
            }
        }

        // 整键双色分段：A→中点、中点→B
        const glm::vec3 dir = pos[gb] - pos[ga];
        const float halfLen = 0.5f * glm::length(dir);
        const glm::vec3 dirN = halfLen > 1e-6f ? dir / (2.0f * halfLen)
                                               : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 mid = pos[ga] + dirN * halfLen;
        const glm::vec4 ca = Representer::colorForAtom(ctx, ga);
        const glm::vec4 cb = Representer::colorForAtom(ctx, gb);

        CylinderInst cyl1;
        cyl1.aPos = glm::vec4(pos[ga], 0.0f);
        cyl1.bPos = glm::vec4(mid, 0.0f);
        cyl1.radius = cylR;
        cyl1.pad = 0.0f;
        cyl1.color = ca;
        cyl1.pickId_pad = glm::uvec2(std::min(ga, gb), 0);
        out.append(Cmd::CylinderInst, &cyl1, sizeof(cyl1));

        CylinderInst cyl2;
        cyl2.aPos = glm::vec4(mid, 0.0f);
        cyl2.bPos = glm::vec4(pos[gb], 0.0f);
        cyl2.radius = cylR;
        cyl2.pad = 0.0f;
        cyl2.color = cb;
        cyl2.pickId_pad = glm::uvec2(std::min(ga, gb), 0);
        out.append(Cmd::CylinderInst, &cyl2, sizeof(cyl2));
    }
}

} // namespace moleculens::engine