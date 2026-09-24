// =============================================================================
// MolecuLens · engine · style/tube/tube.cpp —— Tube 实现
// 规格：backbone 平滑管状，简化卡通，适合快速预览蛋白质结构
// =============================================================================

#include "engine/style/tube/tube.h"

#include <map>

#include <glm/glm.hpp>

#include "engine/rep/representer.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

void drawTube(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    const size_t n = st.elements.size();

    // 按 (chainId, residueId) 提取 backbone CA 原子（与 cartoon 同法）
    struct ResidueCA {
        glm::vec3 ca;
        bool      hasCA = false;
    };
    struct Key {
        int32_t chain;
        int32_t res;
        bool operator<(const Key& o) const {
            return chain != o.chain ? chain < o.chain : res < o.res;
        }
    };
    std::map<Key, ResidueCA> byRes;
    for (size_t i = 0; i < n; ++i) {
        const std::string_view name = ctx.scene->atomName(ctx.instance->structureIndex,
                                                          static_cast<uint32_t>(i));
        if (name != "CA" && name != "CA ") {
            continue;
        }
        const int32_t cid = st.atoms[i].chainId;
        const int32_t rid = st.atoms[i].residueId;
        byRes[{cid, rid}].ca = pos[ctx.globalAtomBase + static_cast<uint32_t>(i)];
        byRes[{cid, rid}].hasCA = true;
    }

    // 按链分组（残基顺序由 map 保证）
    std::vector<std::vector<glm::vec3>> chains;
    int32_t curChain = INT32_MIN;
    for (const auto& [k, r] : byRes) {
        if (!r.hasCA) {
            continue;
        }
        if (k.chain != curChain) {
            chains.emplace_back();
            curChain = k.chain;
        }
        chains.back().push_back(r.ca);
    }

    const float tubeRadius = 0.4f * ctx.rep->radiusScale;

    for (const auto& chain : chains) {
        if (chain.size() < 2) {
            continue;
        }
        // 简化版：直接连接控制点生成圆柱串联（采样沿链逐段）
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            CylinderInst cyl;
            cyl.aPos = glm::vec4(chain[i], 0.0f);
            cyl.bPos = glm::vec4(chain[i + 1], 0.0f);
            cyl.radius = tubeRadius;
            cyl.pad = 0.0f;
            cyl.color = Representer::colorForAtom(ctx, ctx.globalAtomBase
                                                      + static_cast<uint32_t>(i));
            cyl.pickId_pad = glm::uvec2(ctx.globalAtomBase
                                            + static_cast<uint32_t>(i), 0);
            out.append(Cmd::CylinderInst, &cyl, sizeof(cyl));

            // 端帽球（平滑连接）
            SphereInst cap;
            cap.posRadius = glm::vec4(chain[i], tubeRadius);
            cap.color = cyl.color;
            cap.pickId = cyl.pickId_pad;
            cap.pad = glm::uvec2(0, 0);
            out.append(Cmd::SphereInst, &cap, sizeof(cap));
        }
        // 最后一个端帽
        SphereInst cap;
        const uint32_t lastIdx = static_cast<uint32_t>(chain.size() - 1);
        cap.posRadius = glm::vec4(chain.back(), tubeRadius);
        cap.color = Representer::colorForAtom(ctx, ctx.globalAtomBase + lastIdx);
        cap.pickId = glm::uvec2(ctx.globalAtomBase + lastIdx, 0);
        cap.pad = glm::uvec2(0, 0);
        out.append(Cmd::SphereInst, &cap, sizeof(cap));
    }
}

} // namespace moleculens::engine