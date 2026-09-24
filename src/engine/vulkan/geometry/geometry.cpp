// =============================================================================
// MolecuLens · engine · vulkan/geometry/geometry.cpp —— 共享几何与投影实现
// 本文件是 generateIcosphere / generateCylinder / vkProjection 的唯一实现。
// =============================================================================

#include "engine/vulkan/geometry/geometry.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace moleculens::engine {

namespace {

// 细分一次：每条边取中点（按边缓存去重）→ 每三角形 4 分。
void subdivideOnce(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    std::vector<glm::vec3> newVerts = verts;
    std::vector<uint32_t> newIndices;
    newIndices.reserve(indices.size() * 4);
    std::unordered_map<uint64_t, uint32_t> midCache;
    auto mid = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint32_t lo = std::min(a, b), hi = std::max(a, b);
        const uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
        auto it = midCache.find(key);
        if (it != midCache.end()) {
            return it->second;
        }
        const glm::vec3 m = glm::normalize(verts[lo] + verts[hi]);
        const auto idx = static_cast<uint32_t>(newVerts.size());
        newVerts.push_back(m);
        midCache.emplace(key, idx);
        return idx;
    };
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const uint32_t m01 = mid(i0, i1), m12 = mid(i1, i2), m20 = mid(i2, i0);
        newIndices.insert(newIndices.end(),
                          {i0, m01, m20, m01, i1, m12, m20, m12, i2, m01, m12, m20});
    }
    verts.swap(newVerts);
    indices.swap(newIndices);
}

} // namespace

void generateIcosphere(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    // 正二十面体
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const glm::vec3 base[] = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    for (const auto& v : base) {
        verts.push_back(glm::normalize(v));
    }
    const uint32_t faces[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    for (const auto& f : faces) {
        const uint32_t i0 = f[0], i1 = f[1], i2 = f[2];
        const glm::vec3 v0 = verts[i0], v1 = verts[i1], v2 = verts[i2];
        const glm::vec3 m01 = glm::normalize(v0 + v1);
        const glm::vec3 m12 = glm::normalize(v1 + v2);
        const glm::vec3 m20 = glm::normalize(v2 + v0);
        const uint32_t j0 = static_cast<uint32_t>(verts.size());
        const uint32_t j1 = j0 + 1, j2 = j0 + 2;
        verts.push_back(m01);
        verts.push_back(m12);
        verts.push_back(m20);
        indices.insert(indices.end(), {i0, j0, j2, j0, i1, j1, j2, j1, i2, j0, j1, j2});
    }
    subdivideOnce(verts, indices);   // 2 级：80 → 320 三角形
}

void generateCylinder(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    constexpr int kSegments = 8;
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (int i = 0; i < kSegments; ++i) {
        const float a = 6.2831853f * i / kSegments;
        const float ca = std::cos(a), sa = std::sin(a);
        verts.push_back(glm::vec3(ca, -1.0f, sa));
        verts.push_back(glm::vec3(ca, 1.0f, sa));
    }
    for (int i = 0; i < kSegments; ++i) {
        const uint32_t i0 = base + static_cast<uint32_t>(i) * 2;
        const uint32_t i1 = i0 + 1;
        const uint32_t i2 = base + static_cast<uint32_t>((i + 1) % kSegments) * 2;
        const uint32_t i3 = i2 + 1;
        indices.insert(indices.end(), {i0, i1, i2, i1, i3, i2});
    }
}

glm::mat4 vkProjection(const glm::mat4& proj) {
    glm::mat4 m = proj;
    m[1][1] *= -1.0f;
    // 深度范围：glm::perspective 默认 [-1,1] → Vulkan 的 [0,1]。
    // GLM 列主序 m[col][row]：z 行 w 系数在 m[3][2]，w 行 z 系数在 m[2][3]。
    // 故 z 系数' = 0.5*A + 0.5*(-1)（读 m[2][3]），w 系数' = 0.5*B（写 m[3][2]），
    // clip.w 保持 -z 不变。读写方向写反 → clip.w = -0.5z → 几何被远平面裁剪。
    m[2][2] = m[2][2] * 0.5f + m[2][3] * 0.5f;
    m[3][2] = m[3][2] * 0.5f;
    return m;
}

} // namespace moleculens::engine
