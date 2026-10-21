// =============================================================================
// MolecuLens · engine · style/plate/plate.cpp —— Plate 实现
// 规格：每个残基（或单残基分子）一块扁平方板；朝向由原子云的主成分轴决定。
// 为什么是"平板"：参考 TCBG 光合体系示意图——辅基（叶绿素等）画成彩色小方片，
// 而不是球棍/空间填充。它只看"位置 + 朝向 + 归属"，不表达原子级细节。
// 实现：TriMesh 六面体（12 三角形），走既有网格管线，无需新着色器/管线。
// =============================================================================

#include "engine/style/plate/plate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {
namespace {

// 幂迭代求协方差矩阵的主方向（seed 给一个非零初值；退化时返回零向量）
glm::vec3 principalAxis(const glm::mat3& cov, const glm::vec3& seed) {
    glm::vec3 v = seed;
    const float len0 = glm::length(v);
    if (len0 < 1e-6f) {
        return glm::vec3(0.0f);
    }
    v /= len0;
    for (int i = 0; i < 24; ++i) {
        const glm::vec3 next = cov * v;
        const float len = glm::length(next);
        if (len < 1e-7f) {
            return glm::vec3(0.0f);
        }
        v = next / len;
    }
    return v;
}

// 把一块扁平方板（中心 c、三条半轴 hx/hy/hz）展开成 24 顶点 / 36 索引。
// 每个面独立顶点：法线必须是面法线（共用顶点会把盒子画成"圆滑骰子"）。
// 绕序：对每个面取 (u, v) 使 cross(u, v) = 外法线，三角形 0-1-2 / 0-2-3 即 CCW。
void appendPlateBox(std::vector<TriMeshVertex>& verts, std::vector<uint32_t>& indices,
                    const glm::vec3& c, const glm::vec3& hx, const glm::vec3& hy,
                    const glm::vec3& hz, const glm::vec4& color) {
    struct Face {
        glm::vec3 normal;
        glm::vec3 u;
        glm::vec3 v;
        glm::vec3 offset;   // 面中心相对 c 的偏移（±hx / ±hy / ±hz）
    };
    const Face faces[6] = {
        {glm::normalize(hx), hy, hz, hx},
        {-glm::normalize(hx), hz, hy, -hx},
        {glm::normalize(hy), hz, hx, hy},
        {-glm::normalize(hy), hx, hz, -hy},
        {glm::normalize(hz), hx, hy, hz},
        {-glm::normalize(hz), hy, hx, -hz},
    };
    for (const Face& f : faces) {
        const glm::vec3 fc = c + f.offset;
        const glm::vec3 p0 = fc - f.u - f.v;
        const glm::vec3 p1 = fc + f.u - f.v;
        const glm::vec3 p2 = fc + f.u + f.v;
        const glm::vec3 p3 = fc - f.u + f.v;
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (const glm::vec3& p : {p0, p1, p2, p3}) {
            TriMeshVertex tv;
            tv.pos = glm::vec4(p, 0.0f);
            tv.normal = glm::vec4(f.normal, 0.0f);
            tv.color = color;
            verts.push_back(tv);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

} // namespace

void drawPlate(const RepContext& ctx, SceneCommandList& out) {
    if (ctx.scene == nullptr || ctx.instance == nullptr || ctx.mask == nullptr ||
        ctx.rep == nullptr) {
        return;
    }
    if (ctx.instance->structureIndex >= ctx.scene->structures().size()) {
        return;
    }
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (pos == nullptr) {
        return;
    }

    // —— 分组：链 + 残基（单残基分子 = 每个实例一块）——
    std::unordered_map<int64_t, std::vector<uint32_t>> groups;
    for (size_t i = 0; i < st.atoms.size(); ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        const auto& a = st.atoms[i];
        const int64_t key =
            (static_cast<int64_t>(a.chainId) + 1) * 1000000 + (static_cast<int64_t>(a.residueId) + 1);
        groups[key].push_back(static_cast<uint32_t>(i));
    }

    const float s = std::max(0.2f, ctx.rep->radiusScale);
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
    int platesInChunk = 0;

    for (const auto& kv : groups) {
        const std::vector<uint32_t>& atoms = kv.second;
        if (atoms.empty()) {
            continue;
        }
        // 质心
        glm::dvec3 sum(0.0);
        for (uint32_t idx : atoms) {
            const glm::vec3& p = pos[ctx.globalAtomBase + idx];
            sum += glm::dvec3(p);
        }
        const glm::vec3 c = glm::vec3(sum / static_cast<double>(atoms.size()));

        glm::vec3 ax(1.0f, 0.0f, 0.0f);
        glm::vec3 ay(0.0f, 1.0f, 0.0f);
        glm::vec3 az(0.0f, 0.0f, 1.0f);
        if (atoms.size() >= 3) {
            // 协方差 → 主轴：最大方差方向 → 次轴 → 法线
            glm::mat3 cov(0.0f);
            for (uint32_t idx : atoms) {
                const glm::vec3 d = pos[ctx.globalAtomBase + idx] - c;
                cov += glm::outerProduct(d, d);
            }
            cov /= static_cast<float>(atoms.size());

            ax = principalAxis(cov, glm::vec3(1.0f, 0.0f, 0.0f));
            if (glm::length(ax) < 1e-4f) {
                ax = principalAxis(cov, glm::vec3(0.0f, 1.0f, 0.0f));
            }
            if (glm::length(ax) < 1e-4f) {
                ax = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            const float lam1 = glm::dot(ax, cov * ax);
            glm::mat3 cov2 = cov - lam1 * glm::outerProduct(ax, ax);
            ay = principalAxis(cov2, glm::vec3(0.0f, 1.0f, 0.0f));
            if (glm::length(ay) < 1e-4f) {
                ay = principalAxis(cov2, glm::vec3(0.0f, 0.0f, 1.0f));
            }
            if (glm::length(ay) < 1e-4f) {
                // 退化（近似球形/线形）：构造任意正交基
                ay = std::fabs(ax.y) < 0.9f
                         ? glm::normalize(glm::cross(ax, glm::vec3(0, 1, 0)))
                         : glm::normalize(glm::cross(ax, glm::vec3(0, 0, 1)));
            }
            ay = glm::normalize(ay - ax * glm::dot(ay, ax));
            az = glm::normalize(glm::cross(ax, ay));
        } else if (atoms.size() == 2) {
            // 两个原子：连线为长轴，任一垂直方向为短轴。此前直接跳过，
            // 于是双原子分子/离子（以及只有 1 个原子的离子）在 Plate 下完全消失。
            const glm::vec3 d = pos[ctx.globalAtomBase + atoms[1]] -
                                pos[ctx.globalAtomBase + atoms[0]];
            if (glm::length(d) > 1e-5f) {
                ax = glm::normalize(d);
            }
            const glm::vec3 seed =
                std::fabs(ax.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
            ay = glm::normalize(glm::cross(seed, ax));
            az = glm::normalize(glm::cross(ax, ay));
        }
        // 单原子：使用世界轴，尺寸由下方下限给出（0.45 × 0.30 × 0.08）。

        // 沿三轴的半跨度
        float e1 = 0.0f;
        float e2 = 0.0f;
        float e3 = 0.0f;
        for (uint32_t idx : atoms) {
            const glm::vec3 d = pos[ctx.globalAtomBase + idx] - c;
            e1 = std::max(e1, std::fabs(glm::dot(d, ax)));
            e2 = std::max(e2, std::fabs(glm::dot(d, ay)));
            e3 = std::max(e3, std::fabs(glm::dot(d, az)));
        }
        // 平面尺寸按原子云跨度放大一点；厚度给一个薄片下限（太薄会闪烁）
        const glm::vec3 hx = ax * std::max(0.45f, (e1 + 0.35f) * 1.05f) * s;
        const glm::vec3 hy = ay * std::max(0.30f, (e2 + 0.35f) * 1.05f) * s;
        const glm::vec3 hz =
            az * std::clamp((e3 + 0.12f) * 0.22f, 0.08f, 0.30f) * s;

        const uint32_t firstGlobal = ctx.globalAtomBase + atoms.front();
        glm::vec4 color = Representer::colorForAtom(ctx, firstGlobal);
        color.a = 1.0f;
        appendPlateBox(verts, indices, c, hx, hy, hz, color);

        // 分块发射：单命令载荷上限 16 MiB，大体系一面板也没问题，但按 256 块切更稳
        if (++platesInChunk >= 256) {
            out.appendTriMesh(verts, indices);
            verts.clear();
            indices.clear();
            platesInChunk = 0;
        }
    }
    if (!verts.empty() && !indices.empty()) {
        out.appendTriMesh(verts, indices);
    }
}

} // namespace moleculens::engine
