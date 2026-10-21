// =============================================================================
// Moleculens · engine · style/ribbon/ribbon.cpp —— Ribbon 实现
// 规格：薄带状 backbone 表现，仅显示骨架路径
// =============================================================================

#include "engine/style/ribbon/ribbon.h"

#include <glm/glm.hpp>

#include "engine/style/backbone/backbone_trace.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawRibbon(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    // CA 骨架路径（点自带全局原子下标 —— 配色/拾取必须用它，见 backbone_trace.h）
    std::vector<std::vector<CaPoint>> chains;
    extractCaChains(ctx, st, pos, chains);

    // Ribbon：薄带由细圆柱近似（薄，非管状：厚度远小于 Tube 半径）
    const float ribbonThickness = 0.15f * ctx.rep->radiusScale;

    for (const auto& chain : chains) {
        if (chain.size() < 2) {
            continue;
        }
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            const CaPoint& a = chain[i];
            const CaPoint& b = chain[i + 1];
            // 断链不连；选择子集须两端都在掩码内（见 backbone_trace.h）
            if (glm::length(b.p - a.p) > kCaBreakThreshold) {
                continue;
            }
            if (!caSegmentSelected(ctx, a, b)) {
                continue;
            }
            CylinderInst cyl;
            cyl.aPos = glm::vec4(a.p, 0.0f);
            cyl.bPos = glm::vec4(b.p, 0.0f);
            cyl.radius = ribbonThickness;
            cyl.pad = 0.0f;
            cyl.color = Representer::colorForAtom(ctx, a.globalAtom);
            cyl.pickId_pad = glm::uvec2(a.globalAtom, 0);
            out.append(Cmd::CylinderInst, &cyl, sizeof(cyl));
        }
    }
}

} // namespace moleculens::engine
