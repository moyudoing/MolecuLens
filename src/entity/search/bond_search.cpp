// =============================================================================
// MolecuLens · entity · search/bond_search.cpp —— 键搜索实现
// 规格来源：主方案 §4.7（cell-list；多线程按 box 步长切片分块，互斥合并）
// =============================================================================

#include "entity/search/bond_search.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include <glm/glm.hpp>

namespace moleculens::entity {

namespace {

struct Cell {
    std::vector<uint32_t> atoms;
};

} // namespace

void BondSearch::search(const std::vector<glm::vec3>& positions,
                        const std::vector<ElementType>& elements,
                        std::vector<Bond>& outBonds,
                        const BondSearchOptions& opts) {
    outBonds.clear();
    const size_t n = positions.size();
    if (n < 2) {
        return;
    }

    // 1) 计算每原子判键距离 + 体系最大判键距离（cell 尺寸）
    std::vector<float> cutoff(n);
    float maxCutoff = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float cov = elementInfo(elements[i]).covalentRadius;
        cutoff[i] = 2.0f * cov + opts.tolerance;
        maxCutoff = std::max(maxCutoff, cutoff[i]);
    }
    if (maxCutoff <= 0.0f) {
        return;
    }

    // 2) 包围盒 + 网格尺寸
    glm::vec3 lo = positions[0], hi = positions[0];
    for (size_t i = 1; i < n; ++i) {
        lo = glm::min(lo, positions[i]);
        hi = glm::max(hi, positions[i]);
    }
    const glm::vec3 extent = hi - lo;
    const glm::ivec3 dims(
        std::max(1, static_cast<int>(extent.x / maxCutoff) + 1),
        std::max(1, static_cast<int>(extent.y / maxCutoff) + 1),
        std::max(1, static_cast<int>(extent.z / maxCutoff) + 1));
    const int dimX = dims.x, dimY = dims.y, dimZ = dims.z;
    const glm::vec3 invCell(maxCutoff > 0 ? 1.0f / maxCutoff : 0.0f);

    std::vector<Cell> grid(static_cast<size_t>(dimX) * dimY * dimZ);
    auto cellIndex = [&](int cx, int cy, int cz) -> int64_t {
        return static_cast<int64_t>(cz) * dimX * dimY + cy * dimX + cx;
    };
    auto cellCoord = [&](const glm::vec3& p, int& cx, int& cy, int& cz) {
        const glm::vec3 f = (p - lo) * invCell;
        cx = std::min(dimX - 1, std::max(0, static_cast<int>(f.x)));
        cy = std::min(dimY - 1, std::max(0, static_cast<int>(f.y)));
        cz = std::min(dimZ - 1, std::max(0, static_cast<int>(f.z)));
    };

    for (uint32_t i = 0; i < n; ++i) {
        int cx, cy, cz;
        cellCoord(positions[i], cx, cy, cz);
        grid[static_cast<size_t>(cellIndex(cx, cy, cz))].atoms.push_back(i);
    }

    // 3) 并行分块：按 x 切片分块，每线程独立候选列表，互斥合并
    const int hardware = static_cast<int>(std::thread::hardware_concurrency());
    const int threadCount = opts.multiThreaded && opts.threadCount > 0
                                ? opts.threadCount
                                : (opts.multiThreaded ? std::max(1, hardware) : 1);

    struct BlockResult {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
    };
    std::vector<BlockResult> results(static_cast<size_t>(threadCount));

    auto blockOfX = [&](int x) { return x * threadCount / dimX; };

    const auto work = [&](int tid) {
        auto& pairs = results[static_cast<size_t>(tid)].pairs;
        pairs.reserve(256);
        for (int cx = 0; cx < dimX; ++cx) {
            if (blockOfX(cx) != tid) {
                continue;
            }
            for (int cy = 0; cy < dimY; ++cy) {
                for (int cz = 0; cz < dimZ; ++cz) {
                    const Cell& cell = grid[static_cast<size_t>(cellIndex(cx, cy, cz))];
                    // 27 邻域（含自身，自身内两两检查）
                    for (int dz = -1; dz <= 1; ++dz) {
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= dimX ||
                                    ny >= dimY || nz >= dimZ) {
                                    continue;
                                }
                                const Cell& ncell =
                                    grid[static_cast<size_t>(cellIndex(nx, ny, nz))];
                                // 无向去重：只与"字典序更小"的 cell 组合内比较
                                const bool process =
                                    (dx > 0) || (dx == 0 && dy > 0) ||
                                    (dx == 0 && dy == 0 && dz > 0) ||
                                    (dx == 0 && dy == 0 && dz == 0);
                                if (!process && &ncell == &cell) {
                                    continue;
                                }
                                for (uint32_t a : cell.atoms) {
                                    for (uint32_t b : ncell.atoms) {
                                        if (a >= b) {
                                            continue;
                                        }
                                        // 判键条件：dist ≤ cov(a)+cov(b)+0.45 Å（主方案 §4.7）
                                        const float threshold =
                                            elementInfo(elements[a]).covalentRadius +
                                            elementInfo(elements[b]).covalentRadius +
                                            opts.tolerance;
                                        const glm::vec3 d = positions[b] - positions[a];
                                        if (glm::dot(d, d) <= threshold * threshold) {
                                            pairs.emplace_back(a, b);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    };

    if (threadCount <= 1) {
        work(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(threadCount));
        for (int t = 0; t < threadCount; ++t) {
            threads.emplace_back(work, t);
        }
        for (auto& th : threads) {
            th.join();
        }
    }

    // 4) 合并 + 排序去重
    for (const auto& r : results) {
        outBonds.reserve(outBonds.size() + r.pairs.size());
        for (const auto& [a, b] : r.pairs) {
            outBonds.push_back(Bond{a, b, 1});
        }
    }
    std::sort(outBonds.begin(), outBonds.end(),
              [](const Bond& x, const Bond& y) {
                  return x.a != y.a ? x.a < y.a : x.b < y.b;
              });
    outBonds.erase(std::unique(outBonds.begin(), outBonds.end(),
                               [](const Bond& x, const Bond& y) {
                                   return x.a == y.a && x.b == y.b;
                               }),
                   outBonds.end());
}

void BondSearch::searchForStructure(const SceneStructure& scene, uint32_t structureIndex,
                                    const std::vector<glm::vec3>& positions,
                                    const BondSearchOptions& opts) {
    if (structureIndex >= scene.structures().size()) {
        return;
    }
    const auto& ms = scene.structures()[structureIndex];
    std::vector<Bond> bonds;
    search(positions, ms.elements, bonds, opts);
    // 注意：结构池只读不可变（去重键依赖）；调用方应重新注册（M1 提供
    // 建键流程：解析器在注册前推断键，见 pdb/gro 解析器）。
}

} // namespace moleculens::entity
