// =============================================================================
// MoleculeLens · engine · style/diamond/diamond.cpp —— 菱形（八面体宝石）实现
//
// 需求：除了球/棍/线/表面之外，提供一种"示意性"的分子表示 —— 一颗菱形代表
// 一个分子（或残基）。它只表达位置、大小和归属，不表达原子级细节，适合：
//   · 大量小分子/溶剂的粗粒化视图（一眼看清分布与取向）；
//   · 论文示意图里把配体/辅基画成宝石状标记（与 Plate 同一类用途，但更立体）。
//
// 实现：每个 (chain, residue) 一颗八面体，TriMesh 发射（走既有网格管线，
// 无需新着色器/管线）；面法线独立 → 每个棱面有独立高光，看起来像切面宝石。
// =============================================================================

#include "engine/style/diamond/diamond.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {
namespace {

// 追加一颗八面体宝石（12 顶点/面 × 8 面 = 24 顶点、24 索引）。
// 顶点法线是**面法线**且每个面独立顶点：共用顶点会把宝石画成"圆滑的球"。
void appendGem(std::vector<TriMeshVertex>& verts, std::vector<uint32_t>& indices,
               const glm::vec3& c, float r, const glm::vec4& baseColor) {
    const float h = r * 1.28f;    // 上下尖端（略拉长，更像宝石）
    const float e = r * 0.82f;    // 赤道半径
    const glm::vec3 top(c.x, c.y, c.z + h);
    const glm::vec3 bot(c.x, c.y, c.z - h);
    const glm::vec3 eq[4] = {
        glm::vec3(c.x + e, c.y, c.z),
        glm::vec3(c.x, c.y + e, c.z),
        glm::vec3(c.x - e, c.y, c.z),
        glm::vec3(c.x, c.y - e, c.z),
    };
    struct Face {
        glm::vec3 a, b, d;
        float tint;
    };
    const Face faces[8] = {
        {top, eq[0], eq[1], 1.00f}, {top, eq[1], eq[2], 0.90f},
        {top, eq[2], eq[3], 0.82f}, {top, eq[3], eq[0], 0.94f},
        {bot, eq[1], eq[0], 0.86f}, {bot, eq[2], eq[1], 0.96f},
        {bot, eq[3], eq[2], 0.88f}, {bot, eq[0], eq[3], 0.98f},
    };
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        glm::vec3 n = glm::cross(f.b - f.a, f.d - f.a);
        const float len = glm::length(n);
        if (len < 1e-9f) {
            n = glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            n /= len;
        }
        // 绕序确保面朝外（cross 方向与"面中心 - 宝石中心"同向）
        glm::vec3 p0 = f.a, p1 = f.b, p2 = f.d;
        if (glm::dot(n, (p0 + p1 + p2) / 3.0f - c) < 0.0f) {
            std::swap(p1, p2);
            n = -n;
        }
        glm::vec4 col = baseColor;
        col.r = std::min(col.r * f.tint, 1.0f);
        col.g = std::min(col.g * f.tint, 1.0f);
        col.b = std::min(col.b * f.tint, 1.0f);
        col.a = 1.0f;
        for (const glm::vec3& p : {p0, p1, p2}) {
            TriMeshVertex v;
            v.pos = glm::vec4(p, 0.0f);
            v.normal = glm::vec4(n, 0.0f);
            v.color = col;
            verts.push_back(v);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
    }
}

} // namespace

void drawDiamond(const RepContext& ctx, SceneCommandList& out) {
    if (ctx.scene == nullptr || ctx.instance == nullptr || ctx.mask == nullptr ||
        ctx.rep == nullptr || ctx.instance->structureIndex >= ctx.scene->structures().size()) {
        return;
    }
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (pos == nullptr) {
        return;
    }

    // 按 (链, 残基) 分组：GRO 的单残基分子（SOL 等）各自成组，一个分子一颗宝石。
    std::unordered_map<int64_t, std::vector<uint32_t>> groups;
    for (size_t i = 0; i < st.atoms.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        const auto& a = st.atoms[i];
        const int64_t key = (static_cast<int64_t>(a.chainId) + 1) * 1000000 +
                            (static_cast<int64_t>(a.residueId) + 1);
        groups[key].push_back(static_cast<uint32_t>(i));
    }

    const float scale = std::max(0.2f, ctx.rep->radiusScale);
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
    int gemsInChunk = 0;

    for (const auto& kv : groups) {
        const std::vector<uint32_t>& atoms = kv.second;
        if (atoms.empty()) {
            continue;
        }
        // 质心 + 平均色
        glm::dvec3 sum(0.0);
        glm::vec4 colorSum(0.0f);
        for (uint32_t idx : atoms) {
            sum += glm::dvec3(pos[ctx.globalAtomBase + idx]);
            colorSum += Representer::colorForAtom(ctx, ctx.globalAtomBase + idx);
        }
        const glm::vec3 c = glm::vec3(sum / static_cast<double>(atoms.size()));
        glm::vec4 color = colorSum / static_cast<float>(atoms.size());
        if (color.a <= 0.0f) {
            color.a = 1.0f;
        }

        // 尺寸：原子云到质心的最大距离；单原子回落到 vdW 半径。
        float maxDist = 0.0f;
        for (uint32_t idx : atoms) {
            maxDist = std::max(maxDist, glm::length(pos[ctx.globalAtomBase + idx] - c));
        }
        float radius;
        if (atoms.size() == 1) {
            const auto& info = entity::elementInfo(st.elements[atoms.front()]);
            radius = std::max(0.45f, info.vdwRadius * 0.55f);
        } else {
            radius = std::max(0.65f, maxDist * 0.92f + 0.28f);
        }
        radius *= scale;
        if (radius < 0.15f) {
            continue;
        }

        appendGem(verts, indices, c, radius, color);
        if (++gemsInChunk >= 256) {
            out.appendTriMesh(verts, indices);
            verts.clear();
            indices.clear();
            gemsInChunk = 0;
        }
    }
    if (!verts.empty() && !indices.empty()) {
        out.appendTriMesh(verts, indices);
    }
}

} // namespace moleculens::engine
