// =============================================================================
// MolecuLens · engine · style/diamond/diamond.cpp —— 菱形（八面体宝石）实现
//
// 需求（v2.12 修订）：**以非氢原子为单位**，不再以分子/残基为单位。
// 每个重原子（H 以外的原子）一颗八面体宝石，水分子只画 O、蛋白只画 N/C/CA/侧链
// 重原子 —— 既保留“菱形”辨识度，又能看出分子骨架，而不是每分子一颗大宝石。
//
// 实现：逐原子 TriMesh（8 面 / 24 顶点，面法线独立 → 棱面高光），走既有网格管线，
// 无需新着色器/管线。尺寸按元素 vdW 半径缩放，颜色取当前配色。
// =============================================================================

#include "engine/style/diamond/diamond.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {
namespace {

// 追加一颗八面体宝石（8 面 × 3 独立顶点 = 24 顶点 / 24 索引）。
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

inline bool isHydrogen(moleculens::entity::ElementType el) {
    return el == moleculens::entity::ElementType::Hydrogen;
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

    const float scale = std::max(0.2f, ctx.rep->radiusScale);
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
    int gemsInChunk = 0;
    // 全氢结构（如 H2）不能画空白：没有重原子时回退为"所有选中原子"。
    bool hasHeavy = false;
    for (size_t i = 0; i < st.atoms.size() && i < st.elements.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g < ctx.mask->size() && (*ctx.mask)[g] && !isHydrogen(st.elements[i])) {
            hasHeavy = true;
            break;
        }
    }

    for (size_t i = 0; i < st.atoms.size() && i < st.elements.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        const bool heavy = !isHydrogen(st.elements[i]);
        if (hasHeavy && !heavy) {
            continue;   // 非氢原子为单位：H 不画宝石（全氢结构除外）
        }
        const glm::vec3 c = pos[g];
        const auto& info = moleculens::entity::elementInfo(st.elements[i]);
        const float radius =
            std::max(0.18f, info.vdwRadius * (heavy ? 0.52f : 0.40f) * scale);
        glm::vec4 color = Representer::colorForAtom(ctx, g);
        color.a = 1.0f;
        appendGem(verts, indices, c, radius, color);
        if (++gemsInChunk >= 512) {
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
