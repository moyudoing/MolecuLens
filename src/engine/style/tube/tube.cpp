// =============================================================================
// Moleculens · engine · style/tube/tube.cpp —— Tube 实现
// 规格：backbone 平滑管状，简化卡通，适合快速预览蛋白质结构
// =============================================================================

#include "engine/style/tube/tube.h"

#include <glm/glm.hpp>

#include "engine/style/backbone/backbone_trace.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawTube(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    // CA 骨架路径（点自带全局原子下标 —— 配色/拾取必须用它，见 backbone_trace.h）
    std::vector<std::vector<CaPoint>> chains;
    extractCaChains(ctx, st, pos, chains);

    const float tubeRadius = 0.4f * ctx.rep->radiusScale;

    for (const auto& chain : chains) {
        if (chain.size() < 2) {
            continue;
        }
        // 简化版：直接连接控制点生成圆柱串联（采样沿链逐段）
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            const CaPoint& a = chain[i];
            const CaPoint& b = chain[i + 1];
            // 断链不连（否则跨链断裂处会连出一条不存在的长管）
            if (glm::length(b.p - a.p) > kCaBreakThreshold) {
                continue;
            }
            // 选择子集：段两端须都在掩码内（与 stick 对"键"的语义一致）
            if (!caSegmentSelected(ctx, a, b)) {
                continue;
            }
            const glm::vec4 col = Representer::colorForAtom(ctx, a.globalAtom);

            CylinderInst cyl;
            cyl.aPos = glm::vec4(a.p, 0.0f);
            cyl.bPos = glm::vec4(b.p, 0.0f);
            cyl.radius = tubeRadius;
            cyl.pad = 0.0f;
            cyl.color = col;
            cyl.pickId_pad = glm::uvec2(a.globalAtom, 0);
            out.append(Cmd::CylinderInst, &cyl, sizeof(cyl));

            // 端帽球（平滑连接）
            SphereInst cap;
            cap.posRadius = glm::vec4(a.p, tubeRadius);
            cap.color = col;
            cap.pickId = cyl.pickId_pad;
            cap.pad = glm::uvec2(0, 0);
            out.append(Cmd::SphereInst, &cap, sizeof(cap));
        }
        // 末端帽：仅当最后一段确实被画出来（末点自身在选区内）时补
        const CaPoint& last = chain.back();
        const CaPoint& prev = chain[chain.size() - 2];
        if (glm::length(last.p - prev.p) <= kCaBreakThreshold &&
            caSegmentSelected(ctx, prev, last)) {
            SphereInst cap;
            cap.posRadius = glm::vec4(last.p, tubeRadius);
            cap.color = Representer::colorForAtom(ctx, last.globalAtom);
            cap.pickId = glm::uvec2(last.globalAtom, 0);
            cap.pad = glm::uvec2(0, 0);
            out.append(Cmd::SphereInst, &cap, sizeof(cap));
        }
    }
}

} // namespace moleculens::engine
