// =============================================================================
// MolecuLens · engine · style/polyhedron/polyhedron.cpp —— 多面体（二十面体）实现
//
// 每个**非氢原子**一颗二十面体（20 面 / 60 顶点），像切面珠子：
// 顶点按面复制、法线使用面法线，棱线处产生清晰高光；每原子一个确定性朝向，
// 避免整片晶体看起来像统一的球阵。全氢结构回退为所有原子。
// =============================================================================

#include "engine/style/polyhedron/polyhedron.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {
namespace {

inline bool isHydrogenStyle(moleculens::entity::ElementType el) {
    return el == moleculens::entity::ElementType::Hydrogen;
}

// 二十面体 20 个三角面（顶点用黄金比例构造，单位球半径 ~1）。
struct IcoFace {
    glm::vec3 a, b, c;
};

constexpr float kPhi = 1.6180339887498948f;
constexpr float kInvNorm = 0.5257311121191336f;  // 1 / sqrt(1 + phi^2)

std::array<glm::vec3, 12> icosahedronVertices() {
    const float p = kPhi * kInvNorm;
    const float q = 1.0f * kInvNorm;
    std::array<glm::vec3, 12> v = {
        glm::vec3(-q, p, 0), glm::vec3(q, p, 0),
        glm::vec3(-q, -p, 0), glm::vec3(q, -p, 0),
        glm::vec3(0, -q, p), glm::vec3(0, q, p),
        glm::vec3(0, -q, -p), glm::vec3(0, q, -p),
        glm::vec3(p, 0, -q), glm::vec3(p, 0, q),
        glm::vec3(-p, 0, -q), glm::vec3(-p, 0, q),
    };
    return v;
}

constexpr int kIcoFaces[20][3] = {
    {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
    {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
    {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
    {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
};

// 确定性哈希：原子索引 → [0,1)，用于随机朝向，保证同场景每次渲染一致。
float hash01(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n = n ^ (n >> 4);
    n *= 0x27d4eb2du;
    n = n ^ (n >> 15);
    return static_cast<float>(n & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

void appendIcosahedron(std::vector<TriMeshVertex>& verts,
                       std::vector<uint32_t>& indices, const glm::vec3& c,
                       float radius, const glm::vec4& baseColor,
                       uint32_t seed) {
    static const std::array<glm::vec3, 12> unit = icosahedronVertices();
    const glm::mat4 rot =
        glm::rotate(glm::mat4(1.0f), hash01(seed) * 6.2831853f,
                    glm::normalize(glm::vec3(hash01(seed + 1) - 0.5f,
                                             hash01(seed + 2) - 0.5f,
                                             hash01(seed + 3) - 0.5f)));
    std::array<glm::vec3, 12> p;
    for (size_t i = 0; i < unit.size(); ++i) {
        p[i] = glm::vec3(rot * glm::vec4(unit[i] * radius, 0.0f)) + c;
    }
    for (int f = 0; f < 20; ++f) {
        glm::vec3 a = p[kIcoFaces[f][0]];
        glm::vec3 b = p[kIcoFaces[f][1]];
        glm::vec3 d = p[kIcoFaces[f][2]];
        glm::vec3 n = glm::cross(b - a, d - a);
        const float len = glm::length(n);
        if (len < 1e-9f) {
            continue;
        }
        n /= len;
        const glm::vec3 center = (a + b + d) / 3.0f;
        if (glm::dot(n, center - c) < 0.0f) {
            std::swap(b, d);
            n = -n;
        }
        // 面亮度微差，强化"切面珠子"的层次
        const float shade = 0.86f + 0.14f * static_cast<float>(f % 5) / 4.0f;
        glm::vec4 col = baseColor;
        col.r = std::min(col.r * shade, 1.0f);
        col.g = std::min(col.g * shade, 1.0f);
        col.b = std::min(col.b * shade, 1.0f);
        col.a = 1.0f;
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (const glm::vec3& v : {a, b, d}) {
            TriMeshVertex tv;
            tv.pos = glm::vec4(v, 0.0f);
            tv.normal = glm::vec4(n, 0.0f);
            tv.color = col;
            verts.push_back(tv);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
    }
}

} // namespace

void drawPolyhedron(const RepContext& ctx, SceneCommandList& out) {
    if (ctx.scene == nullptr || ctx.instance == nullptr || ctx.mask == nullptr ||
        ctx.rep == nullptr || ctx.instance->structureIndex >= ctx.scene->structures().size()) {
        return;
    }
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (pos == nullptr) {
        return;
    }

    const float scale = std::max(0.2f, ctx.rep->radiusScale);
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
    int gemsInChunk = 0;
    bool hasHeavy = false;
    for (size_t i = 0; i < st.atoms.size() && i < st.elements.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g < ctx.mask->size() && (*ctx.mask)[g] && !isHydrogenStyle(st.elements[i])) {
            hasHeavy = true;
            break;
        }
    }

    for (size_t i = 0; i < st.atoms.size() && i < st.elements.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        const bool heavy = !isHydrogenStyle(st.elements[i]);
        if (hasHeavy && !heavy) {
            continue;
        }
        const auto& info = moleculens::entity::elementInfo(st.elements[i]);
        const float radius =
            std::max(0.18f, info.vdwRadius * (heavy ? 0.55f : 0.42f) * scale);
        glm::vec4 color = Representer::colorForAtom(ctx, g);
        color.a = 1.0f;
        appendIcosahedron(verts, indices, pos[g], radius, color,
                          static_cast<uint32_t>(i) + ctx.instance->structureIndex * 7919u);
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
