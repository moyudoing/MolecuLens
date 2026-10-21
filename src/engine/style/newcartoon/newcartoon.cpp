// =============================================================================
// Moleculens · engine · style/newcartoon/newcartoon.cpp —— NewCartoon 实现
// 规格：改进的卡通模式，基于原 Cartoon 但采样率更高，过渡更平滑
// =============================================================================

#include "engine/style/newcartoon/newcartoon.h"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/style/backbone/backbone_trace.h"
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
SS detectSS(const std::vector<CaPoint>& ca, size_t i) {
    if (i + 2 >= ca.size()) {
        return SS::Coil;
    }
    const float d01 = glm::length(ca[i + 1].p - ca[i].p);
    const float d12 = glm::length(ca[i + 2].p - ca[i + 1].p);
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

    // CA 骨架路径（点自带全局原子下标 —— 配色/拾取必须用它，见 backbone_trace.h）
    std::vector<std::vector<CaPoint>> chains;
    extractCaChains(ctx, st, pos, chains);

    const int samplesPerSegment = 16;   // 每段 16 个采样（比原 Cartoon 更平滑）

    for (const auto& chain : chains) {
        if (chain.size() < 4) {
            continue;
        }

        // Catmull-Rom 样条采样。每个采样点记录自己归属的 CA **全局原子下标**
        // （配色与拾取都以此为键）；原实现传的是采样序号，取到的是结构里
        // 第 N 个原子的元素色，且点选命中完全无关的原子。
        std::vector<glm::vec3>  path;
        std::vector<float>      radii;
        std::vector<glm::vec4>  colors;
        std::vector<uint32_t>   atoms;
        size_t runStart = 0;    // 当前连续段的起点（断链/未选中处断开为独立段）

        // 发射 [begin, end) 内连续采样的圆柱 + 端帽球，并给该段补末端帽
        auto emitRun = [&](size_t begin, size_t end) {
            for (size_t k = begin; k + 1 < end; ++k) {
                const uint32_t pick = atoms[k];
                CylinderInst cyl;
                cyl.aPos = glm::vec4(path[k], 0.0f);
                cyl.bPos = glm::vec4(path[k + 1], 0.0f);
                cyl.radius = 0.5f * (radii[k] + radii[k + 1]);
                cyl.pad = 0.0f;
                cyl.color = colors[k];
                cyl.pickId_pad = glm::uvec2(pick, 0);
                out.append(Cmd::CylinderInst, &cyl, sizeof(cyl));

                SphereInst cap;
                cap.posRadius = glm::vec4(path[k], radii[k]);
                cap.color = colors[k];
                cap.pickId = glm::uvec2(pick, 0);
                cap.pad = glm::uvec2(0, 0);
                out.append(Cmd::SphereInst, &cap, sizeof(cap));
            }
            if (end > begin) {
                const size_t k = end - 1;
                SphereInst cap;
                cap.posRadius = glm::vec4(path[k], radii[k]);
                cap.color = colors[k];
                cap.pickId = glm::uvec2(atoms[k], 0);
                cap.pad = glm::uvec2(0, 0);
                out.append(Cmd::SphereInst, &cap, sizeof(cap));
            }
        };

        for (size_t i = 0; i + 3 < chain.size(); ++i) {
            // 断链或本段未被选择 → 结束当前连续段，从下一个采样点重新开始
            const bool linked =
                glm::length(chain[i + 1].p - chain[i].p) <= kCaBreakThreshold &&
                caSegmentSelected(ctx, chain[i], chain[i + 1]);
            if (!linked) {
                emitRun(runStart, path.size());
                runStart = path.size();
                continue;
            }

            const SS ss = detectSS(chain, i);
            // 二级结构 → 半径
            const float r = ss == SS::Helix   ? 0.9f * ctx.rep->radiusScale
                          : ss == SS::Sheet   ? 0.6f * ctx.rep->radiusScale
                                              : 0.3f * ctx.rep->radiusScale;
            // 本段颜色/拾取归属 CA(i+1)（即段"走向"的那一端）；
            // 每段只求一次色（原实现每个采样点都调一次 colorForAtom）
            const uint32_t atom = chain[i + 1].globalAtom;
            const glm::vec4 col = Representer::colorForAtom(ctx, atom);

            for (int s = 0; s < samplesPerSegment; ++s) {
                const float t = static_cast<float>(s) / samplesPerSegment;
                path.push_back(catmullRom(chain[i].p, chain[i + 1].p,
                                          chain[i + 2].p, chain[i + 3].p, t));
                radii.push_back(r);
                colors.push_back(col);
                atoms.push_back(atom);
            }
        }
        emitRun(runStart, path.size());   // 收尾最后一段
    }
}

} // namespace moleculens::engine
