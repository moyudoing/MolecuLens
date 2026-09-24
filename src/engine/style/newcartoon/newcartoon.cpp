// =============================================================================
// MolecuLens · engine · style/newcartoon/newcartoon.cpp —— NewCartoon 实现
// 规格：改进的卡通模式，基于原 Cartoon 但采样率更高，过渡更平滑
// =============================================================================

#include "engine/style/newcartoon/newcartoon.h"

#include <map>

#include <glm/glm.hpp>

#include "engine/rep/representer.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

namespace {

// 二级结构类型
enum class SS : uint8_t { Coil, Helix, Sheet };

// Catmull-Rom 样条（NewCartoon 更高采样率）
glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1,
                     const glm::vec3& p2, const glm::vec3& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// 几何二级结构判定（简化：按连续 CA 间距）——NewCartoon 骨架仅用 CA
SS detectSS(const std::vector<glm::vec3>& caPos, size_t i) {
    if (i + 2 >= caPos.size()) {
        return SS::Coil;
    }
    const float d01 = glm::length(caPos[i + 1] - caPos[i]);
    const float d12 = glm::length(caPos[i + 2] - caPos[i + 1]);
    // 螺旋：CA 间距约 3.8Å
    if (std::abs(d01 - 3.8f) < 0.5f && std::abs(d12 - 3.8f) < 0.5f) {
        return SS::Helix;
    }
    // 折叠：CA 间距约 3.3–3.5Å（平行 β 折叠）
    if (d01 > 3.2f && d01 < 3.6f && d12 > 3.2f && d12 < 3.6f) {
        return SS::Sheet;
    }
    return SS::Coil;
}

} // namespace

void drawNewCartoon(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    const size_t n = st.elements.size();

    // 按 (chainId, residueId) 提取 backbone CA 原子（与卡通同法）
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

    const int samplesPerSegment = 16;   // 每段 16 个采样（比原 Cartoon 更平滑）

    for (const auto& chain : chains) {
        if (chain.size() < 4) {
            continue;
        }

        // Catmull-Rom 样条采样 + 每点半径
        std::vector<glm::vec3> path;
        std::vector<float>      radii;
        std::vector<glm::vec3>  colors;
        for (size_t i = 0; i + 3 < chain.size(); ++i) {
            const SS ss = detectSS(chain, i);
            // 二级结构 → 半径
            const float r = ss == SS::Helix   ? 0.9f * ctx.rep->radiusScale
                          : ss == SS::Sheet   ? 0.6f * ctx.rep->radiusScale
                                              : 0.3f * ctx.rep->radiusScale;
            for (int s = 0; s < samplesPerSegment; ++s) {
                const float t = static_cast<float>(s) / samplesPerSegment;
                path.push_back(catmullRom(chain[i], chain[i + 1],
                                          chain[i + 2], chain[i + 3], t));
                radii.push_back(r);
                colors.push_back(Representer::colorForAtom(
                    ctx, ctx.globalAtomBase + static_cast<uint32_t>(i + 1)));
            }
        }

        // 圆柱串联 + 端帽球（平滑过渡）
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            const uint32_t pickId = ctx.globalAtomBase
                                    + static_cast<uint32_t>(i);
            CylinderInst cyl;
            cyl.aPos = glm::vec4(path[i], 0.0f);
            cyl.bPos = glm::vec4(path[i + 1], 0.0f);
            cyl.radius = 0.5f * (radii[i] + radii[i + 1]);
            cyl.pad = 0.0f;
            cyl.color = glm::vec4(colors[i], 1.0f);
            cyl.pickId_pad = glm::uvec2(pickId, 0);
            out.append(Cmd::CylinderInst, &cyl, sizeof(cyl));

            SphereInst cap;
            cap.posRadius = glm::vec4(path[i], radii[i]);
            cap.color = glm::vec4(colors[i], 1.0f);
            cap.pickId = glm::uvec2(pickId, 0);
            cap.pad = glm::uvec2(0, 0);
            out.append(Cmd::SphereInst, &cap, sizeof(cap));
        }
        // 最后一个端帽
        if (!path.empty()) {
            const uint32_t pickId =
                ctx.globalAtomBase + static_cast<uint32_t>(path.size() - 1);
            SphereInst cap;
            cap.posRadius = glm::vec4(path.back(), radii.back());
            cap.color = glm::vec4(colors.back(), 1.0f);
            cap.pickId = glm::uvec2(pickId, 0);
            cap.pad = glm::uvec2(0, 0);
            out.append(Cmd::SphereInst, &cap, sizeof(cap));
        }
    }
}

} // namespace moleculens::engine