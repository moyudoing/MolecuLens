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

    // 周期盒子（可选）：3 分量对角线长度（Å）。提供且全 > 0 时，判键位移用
    // 最小镜像（解决跨盒子边界的分子内键被漏判）。注意盒子边界原子的最近邻
    // 经最小镜像可能落于盒子外 —— 网格包围盒仍按原始坐标建，判键位移逐对折算。
    glm::vec3 box(0.0f);
    const bool usePbc = opts.periodicBox != nullptr &&
                        opts.periodicBox[0] > 0.0f && opts.periodicBox[1] > 0.0f &&
                        opts.periodicBox[2] > 0.0f;
    if (usePbc) {
        box = glm::vec3(opts.periodicBox[0], opts.periodicBox[1], opts.periodicBox[2]);
    }
    auto minImage = [&](glm::vec3 d) -> glm::vec3 {
        if (!usePbc) {
            return d;
        }
        // 逐分量最小镜像：d' = d - L*round(d/L)
        d.x -= box.x * std::round(d.x / box.x);
        d.y -= box.y * std::round(d.y / box.y);
        d.z -= box.z * std::round(d.z / box.z);
        return d;
    };

    // 1) 计算每原子判键距离 + 体系最大判键距离（cell 尺寸）。
    //    正确距离判键（主方案 §4.7 参考 VMD guessbonds / RDKit 距离规则）：
    //      bond 判据：dist ≤ cov(a) + cov(b) + tolerance，
    //      其中 tolerance 默认 0.45Å；对含 H 的键收窄为 0.40Å（H 键长更短，
    //      可避免水分子 O–H…O 氢键/跨溶剂假键），金属/Unknown 原子不参与判键
    //      （共价半径 0 → 0 半径假键），原子本身 Unknown → 跳过。
    std::vector<float> cutoff(n);
    float maxCutoff = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const ElementInfo& info = elementInfo(elements[i]);
        const float cov = info.covalentRadius;
        if (elements[i] == ElementType::Unknown || cov <= 0.0f) {
            cutoff[i] = 0.0f;   // Unknown/金属：不参与判键（无共价半径 → 假键）
            continue;
        }
        // 含 H 原子判键阈值略紧（O–H 0.40、C–H 0.38、N–H 0.40 容差更合理）
        const float tol = (elements[i] == ElementType::Hydrogen) ? 0.40f : opts.tolerance;
        cutoff[i] = 2.0f * cov + tol;
        maxCutoff = std::max(maxCutoff, cutoff[i]);
    }
    if (maxCutoff <= 0.0f) {
        return;   // 全部 Unknown / 无共价半径 → 无键（返回空，避免假键）
    }
    if (usePbc) {
        // 周期边界下的邻域半径不得超过盒子半长（否则最小镜像会与自身/复制重合
        // 且判断含糊）；取 min(判键距离, 盒半长) 保证最近邻选取唯一。
        const glm::vec3 half = box * 0.5f;
        const float cellCap = std::min({half.x, half.y, half.z});
        if (cellCap > 1e-3f) {
            maxCutoff = std::min(maxCutoff, cellCap);
        }
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
                                int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                                if (usePbc) {
                                    // 周期边界：邻域按网格维度取模环绕 —— 靠近
                                    // 盒子相对面的 cell 互为邻居，跨边界分子内键
                                    // 的候选原子对才进入判键（位移再按最小镜像折
                                    // 算）。冗余对的最终 sort+unique 去重。
                                    nx = (nx + dimX) % dimX;
                                    ny = (ny + dimY) % dimY;
                                    nz = (nz + dimZ) % dimZ;
                                } else if (nx < 0 || ny < 0 || nz < 0 ||
                                           nx >= dimX || ny >= dimY || nz >= dimZ) {
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
                                        // 判键条件：dist ≤ cov(a)+cov(b)+tol（主方案 §4.7 距离法）
                                        // Unknown/金属原子（共价半径 0）不构成共价键 → 跳过
                                        const ElementInfo& ea = elementInfo(elements[a]);
                                        const ElementInfo& eb = elementInfo(elements[b]);
                                        if (elements[a] == ElementType::Unknown ||
                                            elements[b] == ElementType::Unknown ||
                                            ea.covalentRadius <= 0.0f ||
                                            eb.covalentRadius <= 0.0f) {
                                            continue;
                                        }
                                        // 含 H 的键阈值略紧（避免氢键/跨溶剂假键）
                                        float tol = opts.tolerance;
                                        if (elements[a] == ElementType::Hydrogen ||
                                            elements[b] == ElementType::Hydrogen) {
                                            tol = std::min(tol, 0.40f);
                                        }
                                        const float threshold =
                                            ea.covalentRadius + eb.covalentRadius + tol;
                                        // 周期边界：用最小镜像位移判键（跨盒子边界
                                        // 的分子内键才正确命中，见 bond_search.h）。
                                        const glm::vec3 d = minImage(positions[b] - positions[a]);
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
