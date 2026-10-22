// =============================================================================
// MolecuLens · engine · style/surface/surface.cpp —— vdW 表面（Marching Cubes）
// 规格来源：主方案 §5.7.6（vdW 表面 / SES；输出 TriMesh）
// 实现：标量场 f(p) = min_i(|p-c_i| - r_i)（到原子 vdW 球面的有符号距离），
//       Marching Cubes 抽取 f==0 等值面；顶点法线 = 指向最近原子中心。
// 查找表：MC 四张表统一来自共享头 style/surface/mc_tables.h。
// =============================================================================

#include "engine/style/surface/surface.h"
#include "engine/style/surface/mc_tables.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

namespace {

// 体系表面最近一次生成的网格（运动中冻结时复用；轨迹每帧改坐标时不再重算）。
// 带互斥：导出线程会与主线程并发进入 drawSystemSurface。
struct SystemSurfaceCache {
    std::mutex mtx;
    bool valid = false;
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
};
SystemSurfaceCache g_systemSurfaceCache;
std::atomic<bool> g_freezeSurface{false};

// —— Marching Cubes 查找表（Paul Bourke 公开实现，公共领域）——
// 统一使用共享头 style/surface/mc_tables.h（quicksurf / esp_surface 共用）；
// 本文件历史私有表已与该头逐元素校验一致后移除，行为保持不变。

// —— 标量场计算（G3 加速：每原子体素范围局部 min，多线程分块归约）——
// field(p) = min_i(|p-c_i| - r_i)（到 vdW 球面的有符号距离）；nearest = 最近原子下标。
// 每原子只扫其影响范围（|p-c| ≤ r + margin），避免全原子×全体素 O(N·V)。
void computeVdwSurfaceField(const std::vector<glm::vec3>& centers,
                            const std::vector<float>& radii,
                            const glm::vec3& lo, const glm::ivec3& dims, float sp,
                            std::vector<float>& field, std::vector<int>& nearest) {
    const int nx = dims.x, ny = dims.y, nz = dims.z;
    const size_t nvox = static_cast<size_t>(nx) * ny * nz;
    std::fill(field.begin(), field.end(), 1e30f);
    std::fill(nearest.begin(), nearest.end(), 0);
    const float invSp = 1.0f / sp;
    const float kMargin = 1.2f;   // 与包围盒边距一致（每原子影响半径 = r + margin）
    const unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t n = centers.size();
    struct BlockField {
        std::vector<float> f;
        std::vector<int> n;
    };
    std::vector<BlockField> blocks(nthreads);
    for (auto& b : blocks) {
        b.f.assign(nvox, 1e30f);
        b.n.assign(nvox, 0);
    }
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t]() {
            const size_t i0 = n * t / nthreads;
            const size_t i1 = n * (t + 1) / nthreads;
            auto& fin = blocks[t].f;
            auto& nin = blocks[t].n;
            for (size_t i = i0; i < i1; ++i) {
                const glm::vec3 c = centers[i];
                const float r = radii[i];
                const float infl = r + kMargin;
                const int x0 = std::max(0, static_cast<int>(std::floor((c.x - infl - lo.x) * invSp)));
                const int y0 = std::max(0, static_cast<int>(std::floor((c.y - infl - lo.y) * invSp)));
                const int z0 = std::max(0, static_cast<int>(std::floor((c.z - infl - lo.z) * invSp)));
                const int x1 = std::min(nx - 1, static_cast<int>(std::ceil((c.x + infl - lo.x) * invSp)));
                const int y1 = std::min(ny - 1, static_cast<int>(std::ceil((c.y + infl - lo.y) * invSp)));
                const int z1 = std::min(nz - 1, static_cast<int>(std::ceil((c.z + infl - lo.z) * invSp)));
                for (int z = z0; z <= z1; ++z) {
                    const float dz = lo.z + static_cast<float>(z) * sp - c.z;
                    const size_t zbase = static_cast<size_t>(z) * nx * ny;
                    for (int y = y0; y <= y1; ++y) {
                        const float dy = lo.y + static_cast<float>(y) * sp - c.y;
                        const size_t base = zbase + static_cast<size_t>(y) * nx;
                        float dx = lo.x + static_cast<float>(x0) * sp - c.x;
                        for (int x = x0; x <= x1; ++x) {
                            const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - r;
                            const size_t a = base + static_cast<size_t>(x);
                            if (d < fin[a]) {
                                fin[a] = d;
                                nin[a] = static_cast<int>(i);
                            }
                            dx += sp;
                        }
                    }
                }
            }
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    for (unsigned t = 0; t < nthreads; ++t) {
        auto& fin = blocks[t].f;
        auto& nin = blocks[t].n;
        for (size_t a = 0; a < nvox; ++a) {
            if (fin[a] < field[a]) {
                field[a] = fin[a];
                nearest[a] = nin[a];
            }
        }
    }
}

} // namespace

void drawSurface(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    // 收集选中原子中心与 vdW 半径（另存全局下标：逐顶点着色用）
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
    std::vector<uint32_t> atomGlobal;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        centers.push_back(pos[g]);
        radii.push_back(elementInfo(st.elements[i]).vdwRadius * ctx.rep->radiusScale);
        atomGlobal.push_back(g);
    }
    if (centers.empty()) {
        return;
    }

    // 包围盒 + 边距
    const float margin = 1.2f;
    glm::vec3 lo = centers[0] - glm::vec3(radii[0]);
    glm::vec3 hi = centers[0] + glm::vec3(radii[0]);
    for (size_t i = 1; i < centers.size(); ++i) {
        lo = glm::min(lo, centers[i] - glm::vec3(radii[i]));
        hi = glm::max(hi, centers[i] + glm::vec3(radii[i]));
    }
    lo -= glm::vec3(margin);
    hi += glm::vec3(margin);

    // 网格：优先用表示里的 surfGridSpacing（UI「网格质量」与 CLI --surf-grid 都写这个
    // 字段），再按最大维度与体素总量自适应放大，避免大体系爆内存。原先这里写死
    // 0.8 Å，导致 6k 原子的盒子无论怎么调网格质量都是同一张图。
    constexpr int kMaxDim = 128;
    constexpr size_t kMaxVoxels = 6u * 1024u * 1024u;
    const glm::vec3 size = hi - lo;
    float sp = std::clamp(ctx.rep->surfGridSpacing, 0.15f, 2.0f);
    glm::ivec3 dims(2, 2, 2);
    for (int iter = 0; iter < 64; ++iter) {
        dims = glm::ivec3(std::max(2, static_cast<int>(size.x / sp) + 1),
                          std::max(2, static_cast<int>(size.y / sp) + 1),
                          std::max(2, static_cast<int>(size.z / sp) + 1));
        const int maxDim = std::max({dims.x, dims.y, dims.z});
        const size_t voxels = static_cast<size_t>(dims.x) *
                              static_cast<size_t>(dims.y) *
                              static_cast<size_t>(dims.z);
        if (maxDim <= kMaxDim && voxels <= kMaxVoxels) {
            break;
        }
        sp *= 1.15f;
    }
    const glm::vec3 invSp(1.0f / (size.x / std::max(1, dims.x - 1)),
                          1.0f / (size.y / std::max(1, dims.y - 1)),
                          1.0f / (size.z / std::max(1, dims.z - 1)));
    const auto gridPos = [&](int x, int y, int z) {
        return lo + glm::vec3(x / invSp.x, y / invSp.y, z / invSp.z);
    };

    // 标量场 + 最近原子（每原子体素范围 min 归约；多线程分块 + 归约，见 G3）
    const int nx = dims.x, ny = dims.y, nz = dims.z;
    std::vector<float> field(static_cast<size_t>(nx) * ny * nz);
    std::vector<int> nearest(static_cast<size_t>(nx) * ny * nz);
    const auto idx = [&](int x, int y, int z) {
        return static_cast<size_t>(z) * nx * ny + static_cast<size_t>(y) * nx +
               static_cast<size_t>(x);
    };
    computeVdwSurfaceField(centers, radii, lo, dims, sp, field, nearest);

    // Marching Cubes 抽取 f==0 等值面
    std::vector<glm::vec3> verts;
    std::vector<glm::vec3> vertNrm;   // 与 verts 并行
    std::vector<uint32_t> indices;
    auto interpolate = [&](int x0, int y0, int z0, int x1, int y1, int z1,
                           const glm::vec3& p0, const glm::vec3& p1,
                           glm::vec3* outNrm) {
        const float f0 = field[idx(x0, y0, z0)], f1 = field[idx(x1, y1, z1)];
        const float t = (f1 - f0) != 0.0f ? f0 / (f0 - f1) : 0.5f;
        const glm::vec3 p = glm::mix(p0, p1, glm::clamp(t, 0.0f, 1.0f));
        // 法线 = 指向最近原子中心（vdW 球面法线）
        const int ai = nearest[idx(x0, y0, z0)];
        const glm::vec3 c = centers[static_cast<size_t>(ai)];
        *outNrm = glm::normalize(p - c);
        return p;
    };

    // 12 条边在立方体坐标中的端角索引（用 mc::kEdgeVerts）
    for (int z = 0; z + 1 < nz; ++z) {
        for (int y = 0; y + 1 < ny; ++y) {
            for (int x = 0; x + 1 < nx; ++x) {
                // 8 个角的场值 + 立方体坐标
                glm::vec3 cornerPos[8];
                float cornerVal[8];
                for (int c = 0; c < 8; ++c) {
                    const int cx = x + (mc::kCorner[c][0] > 0.5f ? 1 : 0);
                    const int cy = y + (mc::kCorner[c][1] > 0.5f ? 1 : 0);
                    const int cz = z + (mc::kCorner[c][2] > 0.5f ? 1 : 0);
                    cornerVal[c] = field[idx(cx, cy, cz)];
                    cornerPos[c] = gridPos(cx, cy, cz);
                }
                int cubeIndex = 0;
                for (int c = 0; c < 8; ++c) {
                    if (cornerVal[c] < 0.0f) {
                        cubeIndex |= (1 << c);
                    }
                }
                if (mc::kEdgeTable[cubeIndex] == 0) {
                    continue;
                }
                // 12 条边上的交点
                glm::vec3 edgePos[12];
                glm::vec3 edgeNrm[12];
                for (int e = 0; e < 12; ++e) {
                    if (!(mc::kEdgeTable[cubeIndex] & (1 << e))) {
                        continue;
                    }
                    const int a = mc::kEdgeVerts[e][0], b = mc::kEdgeVerts[e][1];
                    edgePos[e] = interpolate(
                        x + (mc::kCorner[a][0] > 0.5f ? 1 : 0),
                        y + (mc::kCorner[a][1] > 0.5f ? 1 : 0),
                        z + (mc::kCorner[a][2] > 0.5f ? 1 : 0),
                        x + (mc::kCorner[b][0] > 0.5f ? 1 : 0),
                        y + (mc::kCorner[b][1] > 0.5f ? 1 : 0),
                        z + (mc::kCorner[b][2] > 0.5f ? 1 : 0),
                        cornerPos[a], cornerPos[b], &edgeNrm[e]);
                }
                // 三角形
                for (int t = 0; mc::kTriTable[cubeIndex][t] != -1; t += 3) {
                    const int v0 = mc::kTriTable[cubeIndex][t];
                    const int v1 = mc::kTriTable[cubeIndex][t + 1];
                    const int v2 = mc::kTriTable[cubeIndex][t + 2];
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v0]);
                    vertNrm.push_back(edgeNrm[v0]);
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v1]);
                    vertNrm.push_back(edgeNrm[v1]);
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v2]);
                    vertNrm.push_back(edgeNrm[v2]);
                }
            }
        }
    }
    if (verts.empty() || indices.empty()) {
        return;
    }

    // TriMesh 输出（法线 + 逐顶点按原子着色）。
    // 与 VMD 的 Surf「Coloring Method」对齐：顶点色取所属体素最近原子的
    // Representer::colorForAtom（Name 方案下即 C 青/N 蓝/O 红/H 白）。此前这里写死
    // 浅蓝，导致「一个链/一个表面只有一种颜色」——正是用户报告的线框/表面配色问题。
    std::vector<TriMeshVertex> mesh;
    mesh.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const glm::vec3 p = verts[i];
        int cx = static_cast<int>(std::lround((p.x - lo.x) * invSp.x));
        int cy = static_cast<int>(std::lround((p.y - lo.y) * invSp.y));
        int cz = static_cast<int>(std::lround((p.z - lo.z) * invSp.z));
        cx = std::clamp(cx, 0, nx - 1);
        cy = std::clamp(cy, 0, ny - 1);
        cz = std::clamp(cz, 0, nz - 1);
        const int ai = nearest[idx(cx, cy, cz)];
        mesh[i].pos = glm::vec4(p, 0.0f);
        mesh[i].normal = glm::vec4(vertNrm[i], 0.0f);
        mesh[i].color = Representer::colorForAtom(
            ctx, atomGlobal[static_cast<size_t>(ai)]);
    }
    out.appendTriMesh(mesh, indices);
}

// —— 体系级 vdW 表面（G5）：聚合当前体系全部可见原子（含溶剂分子）为一个整体表面。——
// 用于大体系（如溶剂化 md.gro）：不再每分子生成一个独立小表面，而是整体系一个
// 封闭表面（溶剂也在其中），视觉连续、拓扑完整。
void drawSystemSurface(const RepContext& ctx, SceneCommandList& out) {
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    // 运动中冻结：复用上一次网格（见 quicksurf 同名逻辑）。
    if (g_freezeSurface.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_systemSurfaceCache.mtx);
        if (g_systemSurfaceCache.valid && !g_systemSurfaceCache.indices.empty()) {
            out.appendTriMesh(g_systemSurfaceCache.verts, g_systemSurfaceCache.indices);
            return;
        }
    }
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
    std::vector<glm::vec4> atomColors;   // 与 centers 并行：逐顶点着色
    const auto& insts = ctx.scene->instances();
    const size_t total = ctx.frame && ctx.frame->positions ? ctx.frame->positions->size() : 0;
    // 遍历全部实例（体系）的全部原子；排除隐藏实例/隐藏体系
    size_t globalBase = 0;
    for (const auto& inst : insts) {
        uint32_t sysIdx = UINT32_MAX;
        if (inst.systemId != UINT32_MAX) {
            sysIdx = ctx.scene->systemIndexById(inst.systemId);
        }
        const bool hidden = !inst.visible ||
                            (sysIdx != UINT32_MAX && !ctx.scene->systems()[sysIdx].visible);
        const auto& st = ctx.scene->structures()[inst.structureIndex];
        const size_t n = st.elements.size();
        if (hidden) {
            globalBase += n;
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t g = static_cast<uint32_t>(globalBase + i);
            if (g >= total || (ctx.mask && g < ctx.mask->size() && !(*ctx.mask)[g])) {
                continue;
            }
            centers.push_back(pos[static_cast<size_t>(g)]);
            radii.push_back(elementInfo(st.elements[i]).vdwRadius * ctx.rep->radiusScale);
            atomColors.push_back(Representer::colorForAtom(ctx, g));
        }
        globalBase += n;
    }
    if (centers.empty()) {
        return;
    }

    // 包围盒 + 边距
    const float margin = 1.2f;
    glm::vec3 lo = centers[0] - glm::vec3(radii[0]);
    glm::vec3 hi = centers[0] + glm::vec3(radii[0]);
    for (size_t i = 1; i < centers.size(); ++i) {
        lo = glm::min(lo, centers[i] - glm::vec3(radii[i]));
        hi = glm::max(hi, centers[i] + glm::vec3(radii[i]));
    }
    lo -= glm::vec3(margin);
    hi += glm::vec3(margin);

    // 网格：优先用表示里的 surfGridSpacing（UI「网格质量」与 CLI --surf-grid 都写这个
    // 字段），再按最大维度与体素总量自适应放大，避免大体系爆内存。原先这里写死
    // 0.8 Å，导致 6k 原子的盒子无论怎么调网格质量都是同一张图。
    constexpr int kMaxDim = 128;
    constexpr size_t kMaxVoxels = 6u * 1024u * 1024u;
    const glm::vec3 size = hi - lo;
    float sp = std::clamp(ctx.rep->surfGridSpacing, 0.15f, 2.0f);
    glm::ivec3 dims(2, 2, 2);
    for (int iter = 0; iter < 64; ++iter) {
        dims = glm::ivec3(std::max(2, static_cast<int>(size.x / sp) + 1),
                          std::max(2, static_cast<int>(size.y / sp) + 1),
                          std::max(2, static_cast<int>(size.z / sp) + 1));
        const int maxDim = std::max({dims.x, dims.y, dims.z});
        const size_t voxels = static_cast<size_t>(dims.x) *
                              static_cast<size_t>(dims.y) *
                              static_cast<size_t>(dims.z);
        if (maxDim <= kMaxDim && voxels <= kMaxVoxels) {
            break;
        }
        sp *= 1.15f;
    }

    // ⚠ 必须先按网格尺寸分配：computeVdwSurfaceField 按下标 [0,nvox) 写入 field/
    //   nearest，传空 vector 就是**越界写**（实测 6452 原子的整体系 vdW 表面
    //   100% 触发 0xC0000005）。drawSurface 一直是对的，系统级这条路径漏了分配。
    const size_t nvox = static_cast<size_t>(dims.x) * dims.y * dims.z;
    std::vector<float> field(nvox);
    std::vector<int> nearest(nvox);
    computeVdwSurfaceField(centers, radii, lo, dims, sp, field, nearest);

    // MC 抽取（与 drawSurface 相同逻辑）
    const int nx = dims.x, ny = dims.y, nz = dims.z;
    const auto idx = [&](int x, int y, int z) {
        return static_cast<size_t>(z) * nx * ny + static_cast<size_t>(y) * nx +
               static_cast<size_t>(x);
    };
    const auto gridPos = [&](int x, int y, int z) {
        return lo + glm::vec3(static_cast<float>(x) * sp, static_cast<float>(y) * sp,
                             static_cast<float>(z) * sp);
    };
    std::vector<glm::vec3> verts, vertNrm;
    std::vector<uint32_t> indices;
    auto interpolate = [&](int x0, int y0, int z0, int x1, int y1, int z1,
                           const glm::vec3& p0, const glm::vec3& p1, glm::vec3* outNrm) {
        const float f0 = field[idx(x0, y0, z0)], f1 = field[idx(x1, y1, z1)];
        const float t = (f1 - f0) != 0.0f ? f0 / (f0 - f1) : 0.5f;
        const glm::vec3 p = glm::mix(p0, p1, glm::clamp(t, 0.0f, 1.0f));
        const int ai = nearest[idx(x0, y0, z0)];
        const glm::vec3 c = centers[static_cast<size_t>(ai)];
        *outNrm = glm::normalize(p - c);
        return p;
    };
    for (int z = 0; z + 1 < nz; ++z) {
        for (int y = 0; y + 1 < ny; ++y) {
            for (int x = 0; x + 1 < nx; ++x) {
                float cornerVal[8];
                glm::vec3 cornerPos[8];
                for (int c = 0; c < 8; ++c) {
                    const int cx = x + (mc::kCorner[c][0] > 0.5f ? 1 : 0);
                    const int cy = y + (mc::kCorner[c][1] > 0.5f ? 1 : 0);
                    const int cz = z + (mc::kCorner[c][2] > 0.5f ? 1 : 0);
                    cornerVal[c] = field[idx(cx, cy, cz)];
                    cornerPos[c] = gridPos(cx, cy, cz);
                }
                int cubeIndex = 0;
                for (int c = 0; c < 8; ++c) {
                    if (cornerVal[c] < 0.0f) {
                        cubeIndex |= (1 << c);
                    }
                }
                if (mc::kEdgeTable[cubeIndex] == 0) {
                    continue;
                }
                glm::vec3 edgePos[12];
                glm::vec3 edgeNrm[12];
                for (int e = 0; e < 12; ++e) {
                    if (!(mc::kEdgeTable[cubeIndex] & (1 << e))) {
                        continue;
                    }
                    const int a = mc::kEdgeVerts[e][0], b = mc::kEdgeVerts[e][1];
                    edgePos[e] = interpolate(x + (mc::kCorner[a][0] > 0.5f ? 1 : 0),
                                             y + (mc::kCorner[a][1] > 0.5f ? 1 : 0),
                                             z + (mc::kCorner[a][2] > 0.5f ? 1 : 0),
                                             x + (mc::kCorner[b][0] > 0.5f ? 1 : 0),
                                             y + (mc::kCorner[b][1] > 0.5f ? 1 : 0),
                                             z + (mc::kCorner[b][2] > 0.5f ? 1 : 0),
                                             cornerPos[a], cornerPos[b], &edgeNrm[e]);
                }
                for (int t = 0; mc::kTriTable[cubeIndex][t] != -1; t += 3) {
                    const int v0 = mc::kTriTable[cubeIndex][t];
                    const int v1 = mc::kTriTable[cubeIndex][t + 1];
                    const int v2 = mc::kTriTable[cubeIndex][t + 2];
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v0]);
                    vertNrm.push_back(edgeNrm[v0]);
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v1]);
                    vertNrm.push_back(edgeNrm[v1]);
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v2]);
                    vertNrm.push_back(edgeNrm[v2]);
                }
            }
        }
    }
    if (verts.empty() || indices.empty()) {
        return;
    }

    // TriMesh 输出（法线 + 逐顶点按最近原子着色，与 drawSurface 一致）。
    // 这条路径正是"单块超 16 MiB 被静默截断"的现场：大体系整表面能到数十 MB，
    // 必须走 appendTriMesh 的切块发射。
    std::vector<TriMeshVertex> mesh;
    mesh.resize(verts.size());
    const float invSp = 1.0f / std::max(sp, 1e-6f);
    for (size_t i = 0; i < verts.size(); ++i) {
        const glm::vec3 p = verts[i];
        int cx = static_cast<int>(std::lround((p.x - lo.x) * invSp));
        int cy = static_cast<int>(std::lround((p.y - lo.y) * invSp));
        int cz = static_cast<int>(std::lround((p.z - lo.z) * invSp));
        cx = std::clamp(cx, 0, nx - 1);
        cy = std::clamp(cy, 0, ny - 1);
        cz = std::clamp(cz, 0, nz - 1);
        const int ai = nearest[idx(cx, cy, cz)];
        mesh[i].pos = glm::vec4(p, 0.0f);
        mesh[i].normal = glm::vec4(vertNrm[i], 0.0f);
        mesh[i].color = atomColors[static_cast<size_t>(ai)];
    }
    {
        std::lock_guard<std::mutex> lk(g_systemSurfaceCache.mtx);
        g_systemSurfaceCache.verts = mesh;
        g_systemSurfaceCache.indices = indices;
        g_systemSurfaceCache.valid = true;
    }
    out.appendTriMesh(mesh, indices);
}

void surfaceSetFreezeGeometry(bool frozen) {
    g_freezeSurface.store(frozen, std::memory_order_relaxed);
}

bool surfaceFreezeGeometry() {
    return g_freezeSurface.load(std::memory_order_relaxed);
}

} // namespace moleculens::engine
