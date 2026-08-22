// =============================================================================
// MolecuLens · entity · identity/identity.cpp —— 图规范哈希（WL 着色迭代）
// 规格来源：主方案 §4.2（步骤 1–4：直方图 → 初始着色 → 迭代着色 → FNV-1a）
// =============================================================================

#include "entity/identity/identity.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "entity/molecule/molecule.h"

namespace moleculens::entity {

namespace {

inline uint64_t fnv1a64(uint64_t h, uint64_t v) {
    // 每个值一字节混合（保持确定性）
    for (int shift = 0; shift < 64; shift += 8) {
        h ^= (v >> shift) & 0xFFull;
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t fnv1a64Bytes(const std::vector<uint64_t>& values) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : values) {
        h = fnv1a64(h, v);
    }
    return h;
}

} // namespace

StructureKey computeStructureKey(const MoleculeStructure& ms) {
    StructureKey key;
    const size_t n = ms.elements.size();

    // 步骤 1：元素直方图
    for (ElementType e : ms.elements) {
        const uint8_t idx = static_cast<uint8_t>(e);
        if (idx >= 1 && idx < 119) {
            ++key.elementHistogram[idx];
        }
    }

    // 邻接表
    std::vector<std::vector<uint32_t>> adj(n);
    for (const Bond& b : ms.bonds) {
        if (b.a < n && b.b < n) {
            adj[b.a].push_back(b.b);
            adj[b.b].push_back(b.a);
        }
    }

    // 步骤 2：初始着色 = 元素编号
    std::vector<uint64_t> labels(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t idx = static_cast<uint8_t>(ms.elements[i]);
        labels[i] = idx <= 118 ? idx : 0xFFull;
    }

    // 步骤 3：迭代着色（3 轮；每轮 新标签 = hash(自身标签, 排序邻居标签多重集)）
    std::vector<uint64_t> next(n);
    for (int round = 0; round < 3; ++round) {
        for (size_t i = 0; i < n; ++i) {
            std::vector<uint64_t> nb;
            nb.reserve(adj[i].size() + 1);
            nb.push_back(labels[i]);           // 自身标签
            for (uint32_t j : adj[i]) {
                nb.push_back(labels[j]);
            }
            std::sort(nb.begin(), nb.end());   // 邻居多重集排序 → 规范
            uint64_t h = 1469598103934665603ull;
            for (uint64_t v : nb) {
                h = fnv1a64(h, v);
            }
            // 引入轮次区分，避免跨轮标签合并
            next[i] = (h ^ (0x9E3779B97F4A7C15ull * (round + 1)));
        }
        labels.swap(next);
    }

    // 步骤 4：标签 → 计数向量，排序后喂入 64 位 FNV-1a
    std::unordered_map<uint64_t, uint64_t> counts;
    for (uint64_t l : labels) {
        ++counts[l];
    }
    std::vector<uint64_t> vec;
    vec.reserve(counts.size());
    for (const auto& [label, count] : counts) {
        // 组合 (标签, 计数) 为定长字节再混入
        vec.push_back(label);
        vec.push_back(count);
    }
    std::sort(vec.begin(), vec.end());
    key.topologyHash = fnv1a64Bytes(vec);

    return key;
}

} // namespace moleculens::entity
