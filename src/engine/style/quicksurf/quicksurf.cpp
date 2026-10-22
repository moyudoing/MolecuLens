// =============================================================================
// MolecuLens · engine · style/quicksurf/quicksurf.cpp —— QuickSurf 高斯表面实现
// 规格来源：参考 other/QuickSurf（高斯密度和；radscale/gridspacing/isovalue）
// 算法：对原子集合计算高斯密度场（含密度梯度），Marching Cubes 抽取 isovalue
//       等值面 → TriMesh。密度/梯度计算按后端分派：CUDA（启用时）→ AVX2（检测到）
//       → 标量多线程（默认，按原子块并行 + 归约）。
// =============================================================================

#include "engine/style/quicksurf/quicksurf.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

#include "engine/async/surface_jobs.h"
#include "engine/rep/representer.h"
#include "engine/style/quicksurf/quicksurf.h"
#include "engine/style/surface/mc_tables.h"
#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

namespace {

// 高斯截断（标准差的倍数；参考 QuickSurf gausslim≈3.0）
constexpr float kGaussLim = 3.0f;
// 网格最大维度（防爆网格/显存）
constexpr int kMaxGridDim = 128;

// 后端选择状态（Auto = 运行时自动；强制值便于测试/调优）
QuickSurfBackend g_backend = QuickSurfBackend::Auto;
int g_actualBackend = static_cast<int>(QuickSurfBackend::Scalar);

// 异步生成开关（进程级；默认关 = 同步生成，见 quicksurf.h 说明）。
std::atomic<bool> g_asyncEnabled{false};
// 运动中冻结几何（轨迹播放/拖时间轴）：有缓存就直接复用，不按坐标重算。
std::atomic<bool> g_freezeGeometry{false};

// —— 整体系表面网格缓存（多帧） ——
// 动机：
//   1) 命令流在**任何**表示变更（含只改材质/透明度/配色）时整条重建，而表面
//      生成是其中最贵的一步（实测 md.gro 整体系 QuickSurf ~197 ms / 19.8 MB）。
//      缓存键只含几何输入，材质改动直接命中。
//   2) 轨迹逐帧播放：当前帧的表面在后台算完后存入本缓存，同时预取后续帧；
//      播放到该帧时直接命中 ← 表面随轨迹逐帧正确切换，主线程不阻塞。
// 容量按帧数限制（环形淘汰），每帧网格约 8–20 MB，24 帧约 0.2–0.5 GB。
struct CachedSurface {
    uint64_t key = 0;
    uint64_t lastUse = 0;
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
};
struct SurfaceMeshCache {
    std::mutex mtx;
    std::unordered_map<uint64_t, std::shared_ptr<CachedSurface>> entries;
    uint64_t tick = 0;
    uint64_t lastKey = 0;
    size_t capacity = 24;       // 约 0.2–0.5 GB 上限（按实测网格大小）
    uint64_t hits = 0;
    uint64_t misses = 0;
};
SurfaceMeshCache g_systemMeshCache;

// 取缓存项（调用方需持锁）。同时刷新 LRU 时间戳。
std::shared_ptr<CachedSurface> findCachedSurfaceLocked(uint64_t key) {
    auto it = g_systemMeshCache.entries.find(key);
    if (it == g_systemMeshCache.entries.end()) {
        return nullptr;
    }
    it->second->lastUse = ++g_systemMeshCache.tick;
    g_systemMeshCache.lastKey = key;
    return it->second;
}

bool hasCachedSurface(uint64_t key) {
    std::lock_guard<std::mutex> lk(g_systemMeshCache.mtx);
    return g_systemMeshCache.entries.find(key) != g_systemMeshCache.entries.end();
}

// 发射网格（缓存命中与未命中共用同一出口，避免两条路径的切块/序列化行为漂移）
void emitQuickSurfMesh(const std::vector<TriMeshVertex>& verts,
                       const std::vector<uint32_t>& indices, SceneCommandList& out) {
    out.appendTriMesh(verts, indices);
}

// 计算网格原点/维度/实际间距（含边距，等值面不被裁剪）
bool computeGrid(const std::vector<glm::vec3>& centers,
                 const std::vector<float>& radii,
                 float radscale, float gridspacing,
                 glm::vec3& origin, glm::ivec3& dims, float& sp) {
    if (centers.empty()) {
        return false;
    }
    glm::vec3 lo = centers[0] - glm::vec3(radii[0] * radscale);
    glm::vec3 hi = centers[0] + glm::vec3(radii[0] * radscale);
    float maxrad = radii[0];
    for (size_t i = 1; i < centers.size(); ++i) {
        const float sr = radii[i] * radscale;
        lo = glm::min(lo, centers[i] - glm::vec3(sr));
        hi = glm::max(hi, centers[i] + glm::vec3(sr));
        maxrad = std::max(maxrad, radii[i]);
    }
    // 边距（参考 QuickSurf gridpadding = radscale * maxrad * 1.70）
    const float pad = radscale * maxrad * 1.70f;
    lo -= glm::vec3(pad);
    hi += glm::vec3(pad);
    const glm::vec3 size = hi - lo;
    sp = std::max(gridspacing, 0.05f);
    // 维度上限：超限则加粗间距
    int maxDim = 0;
    for (int i = 0; i < 3; ++i) {
        maxDim = std::max(maxDim, static_cast<int>(size[i] / sp) + 1);
    }
    if (maxDim > kMaxGridDim) {
        sp *= static_cast<float>(maxDim) / static_cast<float>(kMaxGridDim);
    }
    dims = glm::ivec3(std::max(2, static_cast<int>(size.x / sp) + 1),
                      std::max(2, static_cast<int>(size.y / sp) + 1),
                      std::max(2, static_cast<int>(size.z / sp) + 1));
    origin = lo;
    return true;
}

// 标量密度核：处理 [a0,a1) 号原子（xyzr 每原子 4 float），累加 density/colormap + grad
void densityAtomRangeScalar(const float* xyzr, const float* colors, int a0, int a1,
                            float radscale, float gridspacing, float gausslim,
                            const float origin[3], const int dims[3],
                            float* density, float* grad, float* colormap) {
    const int nx = dims[0], ny = dims[1], nz = dims[2];
    const float invgs = 1.0f / gridspacing;
    for (int i = a0; i < a1; ++i) {
        const float cx = xyzr[i * 4], cy = xyzr[i * 4 + 1], cz = xyzr[i * 4 + 2];
        const float r = xyzr[i * 4 + 3] * radscale;
        if (r < 1e-4f) {
            continue;
        }
        const float acr = colors ? colors[i * 3] : 1.0f;
        const float acg = colors ? colors[i * 3 + 1] : 1.0f;
        const float acb = colors ? colors[i * 3 + 2] : 1.0f;
        const float arinv = 1.0f / (2.0f * r * r);
        const float radlim = gausslim * r;
        const float radlim2 = radlim * radlim;
        const int x0 = std::max(0, static_cast<int>(std::floor((cx - radlim - origin[0]) * invgs)));
        const int x1 = std::min(nx - 1, static_cast<int>(std::ceil((cx + radlim - origin[0]) * invgs)));
        const int y0 = std::max(0, static_cast<int>(std::floor((cy - radlim - origin[1]) * invgs)));
        const int y1 = std::min(ny - 1, static_cast<int>(std::ceil((cy + radlim - origin[1]) * invgs)));
        const int z0 = std::max(0, static_cast<int>(std::floor((cz - radlim - origin[2]) * invgs)));
        const int z1 = std::min(nz - 1, static_cast<int>(std::ceil((cz + radlim - origin[2]) * invgs)));
        for (int z = z0; z <= z1; ++z) {
            const float dz = origin[2] + static_cast<float>(z) * gridspacing - cz;
            const size_t zbase = static_cast<size_t>(z) * nx * ny;
            for (int y = y0; y <= y1; ++y) {
                const float dy = origin[1] + static_cast<float>(y) * gridspacing - cy;
                const float dy2dz2 = dy * dy + dz * dz;
                if (dy2dz2 >= radlim2) {
                    continue;
                }
                const size_t base = zbase + static_cast<size_t>(y) * nx;
                float dx = origin[0] + static_cast<float>(x0) * gridspacing - cx;
                for (int x = x0; x <= x1; ++x) {
                    const float r2 = dx * dx + dy2dz2;
                    const float g = std::exp(-r2 * arinv);
                    const size_t addr = base + static_cast<size_t>(x);
                    density[addr] += g;
                    // 外向法线 = -∂density/∂p = 2·arinv·(c-p)·g
                    grad[3 * addr + 0] +=
                        2.0f * arinv * (cx - (origin[0] + static_cast<float>(x) * gridspacing)) * g;
                    grad[3 * addr + 1] +=
                        2.0f * arinv * (cy - (origin[1] + static_cast<float>(y) * gridspacing)) * g;
                    grad[3 * addr + 2] +=
                        2.0f * arinv * (cz - (origin[2] + static_cast<float>(z) * gridspacing)) * g;
                    if (colormap) {
                        colormap[3 * addr + 0] += g * acr;
                        colormap[3 * addr + 1] += g * acg;
                        colormap[3 * addr + 2] += g * acb;
                    }
                    dx += gridspacing;
                }
            }
        }
    }
}

// 多线程标量密度（按原子块并行 + 各线程局部 map 归约）
void computeDensityScalarMT(const float* xyzr, int natoms, const float* colors,
                            int a0, int a1, float radscale, float gridspacing,
                            float gausslim, const float origin[3], const int dims[3],
                            float* density, float* grad, float* colormap) {
    (void)natoms;
    const size_t nvoxTmp = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    const unsigned hwTmp = std::max(1u, std::thread::hardware_concurrency());
    // 小网格不值得开线程：每个线程至少 64K 体素（2042 个小分子各 18³ 的场景，
    // 每次调用新建 16 个线程比计算本身还贵）
    const unsigned nthreads =
        std::min(hwTmp, static_cast<unsigned>(std::max<size_t>(1, nvoxTmp / 65536)));
    const size_t nvox = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    const int span = a1 - a0;
    std::vector<std::vector<float>> dens(nthreads, std::vector<float>(nvox, 0.0f));
    std::vector<std::vector<float>> grds(nthreads, std::vector<float>(nvox * 3, 0.0f));
    std::vector<std::vector<float>> cols(
        nthreads, colormap ? std::vector<float>(nvox * 3, 0.0f) : std::vector<float>());
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t]() {
            const int ta0 = a0 + span * static_cast<int>(t) / static_cast<int>(nthreads);
            const int ta1 = a0 + span * static_cast<int>(t + 1) / static_cast<int>(nthreads);
            densityAtomRangeScalar(xyzr, colors, ta0, ta1, radscale, gridspacing,
                                   gausslim, origin, dims, dens[t].data(),
                                   grds[t].data(), colormap ? cols[t].data() : nullptr);
        });
    }
    for (auto& th : pool) {
        th.join();
    }
    for (unsigned t = 0; t < nthreads; ++t) {
        for (size_t i = 0; i < nvox; ++i) {
            density[i] += dens[t][i];
        }
        for (size_t i = 0; i < nvox * 3; ++i) {
            grad[i] += grds[t][i];
        }
        if (colormap) {
            for (size_t i = 0; i < nvox * 3; ++i) {
                colormap[i] += cols[t][i];
            }
        }
    }
}

// 后端强制（调试/基准）：MOLECULENS_QUICKSURF_BACKEND=auto|scalar|avx2|cuda
// 只在首次分派时读取一次，之后沿用 g_backend（不覆盖程序里显式调用的
// quickSurfSetBackend）。
void applyBackendEnvOnce() {
    static const bool kApplied = [] {
        const char* v = std::getenv("MOLECULENS_QUICKSURF_BACKEND");
        if (!v || !*v) {
            return true;
        }
        std::string s(v);
        for (auto& ch : s) {
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        }
        if (s == "scalar" || s == "1") {
            quickSurfSetBackend(static_cast<int>(QuickSurfBackend::Scalar));
        } else if (s == "avx2" || s == "2") {
            quickSurfSetBackend(static_cast<int>(QuickSurfBackend::Avx2));
        } else if (s == "cuda" || s == "3") {
            quickSurfSetBackend(static_cast<int>(QuickSurfBackend::Cuda));
        }
        return true;
    }();
    (void)kApplied;
}

// 后端分派（运行时自动选择：CUDA → AVX2 → 标量；或 quickSurfSetBackend 强制）
void computeDensityDispatch(const float* xyzr, int natoms, const float* colors,
                            float radscale, float gridspacing, float gausslim,
                            const float origin[3], const int dims[3],
                            float* density, float* grad, float* colormap) {
    applyBackendEnvOnce();
    QuickSurfBackend sel = g_backend;
    if (sel == QuickSurfBackend::Auto) {
#if defined(MOLECULENS_QUICKSURF_CUDA)
        if (cudaKernelAvailable()) {
            sel = QuickSurfBackend::Cuda;
        } else
#endif
        if (avx2KernelAvailable()) {
            sel = QuickSurfBackend::Avx2;
        } else {
            sel = QuickSurfBackend::Scalar;
        }
    }
    g_actualBackend = static_cast<int>(sel);
    spdlog::debug("[quicksurf] density backend={} atoms={} grid={}x{}x{}", g_actualBackend,
                  natoms, dims[0], dims[1], dims[2]);
    switch (sel) {
        case QuickSurfBackend::Cuda:
#if defined(MOLECULENS_QUICKSURF_CUDA)
            computeDensityCuda(xyzr, natoms, colors, radscale, gridspacing, gausslim,
                               origin, dims, density, grad, colormap);
            break;
#else
            // 未启用 CUDA 编译：回退到 AVX2/标量
#endif
        case QuickSurfBackend::Avx2:
            computeDensityAvx2(xyzr, natoms, colors, 0, natoms, radscale, gridspacing,
                               gausslim, origin, dims, density, grad, colormap);
            break;
        case QuickSurfBackend::Scalar:
        default:
            computeDensityScalarMT(xyzr, natoms, colors, 0, natoms, radscale,
                                   gridspacing, gausslim, origin, dims, density, grad,
                                   colormap);
            break;
    }
}

// Marching Cubes 抽取 density == isovalue 等值面（法线 = 密度梯度，颜色 = 密度加权原子色）
bool extractSurface(const std::vector<float>& density,
                    const std::vector<float>& grad,
                    const std::vector<float>& colormap,
                    bool hasColor, float isovalue,
                    const glm::vec3& origin, const glm::ivec3& dims,
                    float gridspacing, QuickSurfMesh& out) {
    const int nx = dims.x, ny = dims.y, nz = dims.z;
    const auto idx = [&](int x, int y, int z) {
        return static_cast<size_t>(z) * nx * ny + static_cast<size_t>(y) * nx +
               static_cast<size_t>(x);
    };
    const auto pos = [&](int x, int y, int z) {
        return origin + glm::vec3(static_cast<float>(x) * gridspacing,
                                  static_cast<float>(y) * gridspacing,
                                  static_cast<float>(z) * gridspacing);
    };
    auto& verts = out.positions;
    auto& nrm = out.normals;
    auto& colors = out.colors;
    auto& indices = out.indices;
    verts.clear();
    nrm.clear();
    colors.clear();
    indices.clear();

    // Marching Cubes 按 z 层并行：每个 z 层写自己的局部缓冲，最后按 z 顺序拼接。
    // 三角形发射顺序与串行版完全一致（同一层内 y、x 递增），因此结果逐位相同。
    const int nzs = nz - 1;
    if (nzs <= 0) {
        return false;
    }
    unsigned nt = 1;
    const size_t nvoxTmp = static_cast<size_t>(nx) * ny * nz;
    if (nvoxTmp >= 65536) {
        nt = std::max(1u, std::thread::hardware_concurrency());
        if (const char* tv = std::getenv("MOLECULENS_QUICKSURF_THREADS")) {
            const int n = std::atoi(tv);
            if (n > 0) {
                nt = std::min(nt, static_cast<unsigned>(n));
            }
        }
        nt = std::min(nt, static_cast<unsigned>(nzs));
    }

    struct SliceOut {
        std::vector<glm::vec3> v, n, c;
        std::vector<uint32_t> idx;   // 层内局部索引（0 起）
    };
    std::vector<SliceOut> slices(static_cast<size_t>(nzs));

    auto emitSlice = [&](int z, SliceOut& o) {
        for (int y = 0; y + 1 < ny; ++y) {
            for (int x = 0; x + 1 < nx; ++x) {
                float val[8];
                glm::vec3 gpos[8], gnorm[8], gcol[8];
                for (int c = 0; c < 8; ++c) {
                    const int cx = x + (mc::kCorner[c][0] > 0.5f ? 1 : 0);
                    const int cy = y + (mc::kCorner[c][1] > 0.5f ? 1 : 0);
                    const int cz = z + (mc::kCorner[c][2] > 0.5f ? 1 : 0);
                    const size_t a = idx(cx, cy, cz);
                    val[c] = density[a];
                    gpos[c] = pos(cx, cy, cz);
                    gnorm[c] = glm::vec3(grad[3 * a], grad[3 * a + 1], grad[3 * a + 2]);
                    if (hasColor) {
                        gcol[c] = glm::vec3(colormap[3 * a], colormap[3 * a + 1],
                                            colormap[3 * a + 2]);
                    }
                }
                int cubeIndex = 0;
                for (int c = 0; c < 8; ++c) {
                    if (val[c] >= isovalue) {
                        cubeIndex |= (1 << c);
                    }
                }
                if (mc::kEdgeTable[cubeIndex] == 0) {
                    continue;
                }
                glm::vec3 edgePos[12], edgeNrm[12], edgeCol[12];
                for (int e = 0; e < 12; ++e) {
                    if (!(mc::kEdgeTable[cubeIndex] & (1 << e))) {
                        continue;
                    }
                    const int a = mc::kEdgeVerts[e][0], b = mc::kEdgeVerts[e][1];
                    const float denom = val[b] - val[a];
                    const float t = denom != 0.0f ? (isovalue - val[a]) / denom : 0.5f;
                    const float tt = glm::clamp(t, 0.0f, 1.0f);
                    edgePos[e] = glm::mix(gpos[a], gpos[b], tt);
                    edgeNrm[e] = glm::normalize(glm::mix(gnorm[a], gnorm[b], tt));
                    if (hasColor) {
                        // 颜色按密度归一化（表面处 density≈isovalue → 除以 isovalue）
                        edgeCol[e] = glm::mix(gcol[a], gcol[b], tt) / isovalue;
                    }
                }
                for (int t = 0; mc::kTriTable[cubeIndex][t] != -1; t += 3) {
                    for (int k = 0; k < 3; ++k) {
                        const int vi = mc::kTriTable[cubeIndex][t + k];
                        o.idx.push_back(static_cast<uint32_t>(o.v.size()));
                        o.v.push_back(edgePos[vi]);
                        o.n.push_back(edgeNrm[vi]);
                        if (hasColor) {
                            o.c.push_back(edgeCol[vi]);
                        }
                    }
                }
            }
        }
    };

    if (nt <= 1) {
        for (int z = 0; z < nzs; ++z) {
            emitSlice(z, slices[static_cast<size_t>(z)]);
        }
    } else {
        std::atomic<int> next{0};
        auto worker = [&]() {
            for (;;) {
                const int z = next.fetch_add(1, std::memory_order_relaxed);
                if (z >= nzs) {
                    break;
                }
                emitSlice(z, slices[static_cast<size_t>(z)]);
            }
        };
        std::vector<std::thread> ths;
        ths.reserve(nt);
        for (unsigned i = 0; i < nt; ++i) {
            ths.emplace_back(worker);
        }
        for (auto& t : ths) {
            t.join();
        }
    }

    // 按 z 顺序拼接（局部索引整体平移）
    uint32_t base = 0;
    for (auto& s : slices) {
        if (s.v.empty()) {
            continue;
        }
        indices.reserve(indices.size() + s.idx.size());
        for (uint32_t i : s.idx) {
            indices.push_back(base + i);
        }
        verts.insert(verts.end(), s.v.begin(), s.v.end());
        nrm.insert(nrm.end(), s.n.begin(), s.n.end());
        if (hasColor) {
            colors.insert(colors.end(), s.c.begin(), s.c.end());
        }
        base += static_cast<uint32_t>(s.v.size());
    }
    return !verts.empty() && !indices.empty();
}

} // namespace

void quickSurfSetBackend(int backend) {
    g_backend = static_cast<QuickSurfBackend>(backend);
}

int quickSurfBackend() {
    return g_actualBackend;
}

bool quickSurfMesh(const std::vector<glm::vec3>& centers,
                   const std::vector<float>& radii,
                   float radscale, float gridspacing, float isovalue,
                   QuickSurfMesh& out) {
    glm::vec3 origin;
    glm::ivec3 dims;
    float sp = 0.0f;
    if (!computeGrid(centers, radii, radscale, gridspacing, origin, dims, sp)) {
        return false;
    }
    const size_t nvox = static_cast<size_t>(dims.x) * dims.y * dims.z;
    std::vector<float> density(nvox, 0.0f);
    std::vector<float> grad(nvox * 3, 0.0f);
    std::vector<float> colormap;
    // 打包 xyzr（中心 + 半径）
    std::vector<float> xyzr(centers.size() * 4);
    for (size_t i = 0; i < centers.size(); ++i) {
        xyzr[i * 4 + 0] = centers[i].x;
        xyzr[i * 4 + 1] = centers[i].y;
        xyzr[i * 4 + 2] = centers[i].z;
        xyzr[i * 4 + 3] = radii[i];
    }
    const float o[3] = {origin.x, origin.y, origin.z};
    const int d[3] = {dims.x, dims.y, dims.z};
    const auto t0 = std::chrono::steady_clock::now();
    computeDensityDispatch(xyzr.data(), static_cast<int>(centers.size()), nullptr,
                           radscale, sp, kGaussLim, o, d, density.data(), grad.data(),
                           nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    const bool ok = extractSurface(density, grad, colormap, false, isovalue, origin, dims,
                                   sp, out);
    const auto t2 = std::chrono::steady_clock::now();
    spdlog::info("[quicksurf] atoms={} grid={}x{}x{} sp={} density={:.1f}ms mc={:.1f}ms "
            "tris={}",
            centers.size(), dims.x, dims.y, dims.z, sp,
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::chrono::duration<double, std::milli>(t2 - t1).count(),
            out.indices.size() / 3);
    return ok;
}

void drawQuickSurf(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t n = st.elements.size();
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
    std::vector<glm::vec3> colors;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        centers.push_back(pos[g]);
        radii.push_back(elementInfo(st.elements[i]).vdwRadius * ctx.rep->radiusScale);
        const glm::vec4 c = Representer::colorForAtom(ctx, g);
        colors.push_back(glm::vec3(c));
    }
    if (centers.empty()) {
        return;
    }
    // 密度/梯度/颜色计算（后端自动选择）
    glm::vec3 origin;
    glm::ivec3 dims;
    float sp = 0.0f;
    if (!computeGrid(centers, radii, 1.0f, ctx.rep->surfGridSpacing, origin, dims, sp)) {
        return;
    }
    const size_t nvox = static_cast<size_t>(dims.x) * dims.y * dims.z;
    std::vector<float> density(nvox, 0.0f);
    std::vector<float> grad(nvox * 3, 0.0f);
    std::vector<float> colormap(nvox * 3, 0.0f);
    // 打包 xyzr + 扁平颜色（radscale 已体现在 radii（radiusScale×vdwRadius）中 → 传 1.0）
    std::vector<float> xyzr(centers.size() * 4);
    std::vector<float> flatColors(colors.size() * 3);
    for (size_t i = 0; i < centers.size(); ++i) {
        xyzr[i * 4 + 0] = centers[i].x;
        xyzr[i * 4 + 1] = centers[i].y;
        xyzr[i * 4 + 2] = centers[i].z;
        xyzr[i * 4 + 3] = radii[i];
        flatColors[i * 3 + 0] = colors[i].x;
        flatColors[i * 3 + 1] = colors[i].y;
        flatColors[i * 3 + 2] = colors[i].z;
    }
    const float o[3] = {origin.x, origin.y, origin.z};
    const int d[3] = {dims.x, dims.y, dims.z};
    computeDensityDispatch(xyzr.data(), static_cast<int>(centers.size()),
                           flatColors.data(), 1.0f, sp, kGaussLim, o, d,
                           density.data(), grad.data(), colormap.data());

    QuickSurfMesh mesh;
    if (!extractSurface(density, grad, colormap, true, ctx.rep->surfIsovalue, origin,
                        dims, sp, mesh)) {
        return;
    }

    // TriMesh 输出（法线 + 密度加权原子色）
    // 经 appendTriMesh 发射（按单命令载荷上限自动切块）：整体系 QuickSurf 的
    // 网格可达数十 MB，单块超限会让命令流在解码时被静默截断。
    std::vector<TriMeshVertex> verts;
    verts.resize(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        verts[i].pos = glm::vec4(mesh.positions[i], 0.0f);
        verts[i].normal = glm::vec4(mesh.normals[i], 0.0f);
        // 密度加权原子色（QuickSurf 体积色）；无则浅蓝
        const glm::vec3 c =
            i < mesh.colors.size() ? mesh.colors[i] : glm::vec3(0.55f, 0.75f, 0.95f);
        verts[i].color = glm::vec4(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
    }
    out.appendTriMesh(verts, mesh.indices);
}

// —— 整体系 QuickSurf：计算输入与纯 CPU 计算（可安全跨线程）——
// 任务体只持有此结构（纯数值拷贝），不引用 RepContext/场景/命令流 —— 那些都属于
// 主线程，跨线程访问会数据竞争。工作线程算完把结果写进带互斥的网格缓存。
struct SystemQuickSurfInput {
    std::vector<float> xyzr;        // 每原子 4 float：cx, cy, cz, radius（Å）
    std::vector<float> flatColors;  // 每原子 3 float：密度加权原子色
    glm::vec3 origin{0.0f};         // 网格原点（Å）
    glm::ivec3 dims{0};             // 网格维度
    float spacing = 0.0f;           // 实际网格间距（Å）
    float isovalue = 0.0f;          // 等值面阈值
};

// 纯 CPU：高斯密度场 → Marching Cubes → TriMesh 顶点/索引。
// 线程安全：只读输入、只写局部缓冲，不触碰任何全局可变状态 → 可在工作线程调用。
bool buildSystemQuickSurfMesh(const SystemQuickSurfInput& in,
                              std::vector<TriMeshVertex>& verts,
                              std::vector<uint32_t>& indices) {
    const int atomCount = static_cast<int>(in.xyzr.size() / 4);
    const size_t nvox = static_cast<size_t>(in.dims.x) * in.dims.y * in.dims.z;
    if (atomCount <= 0 || nvox == 0) {
        return false;
    }
    std::vector<float> density(nvox, 0.0f);
    std::vector<float> grad(nvox * 3, 0.0f);
    std::vector<float> colormap(nvox * 3, 0.0f);
    const float o[3] = {in.origin.x, in.origin.y, in.origin.z};
    const int d[3] = {in.dims.x, in.dims.y, in.dims.z};
    const auto t0 = std::chrono::steady_clock::now();
    computeDensityDispatch(in.xyzr.data(), atomCount, in.flatColors.data(), 1.0f,
                           in.spacing, kGaussLim, o, d, density.data(), grad.data(),
                           colormap.data());
    const auto t1 = std::chrono::steady_clock::now();

    QuickSurfMesh mesh;
    if (!extractSurface(density, grad, colormap, true, in.isovalue, in.origin, in.dims,
                        in.spacing, mesh)) {
        return false;
    }
    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    spdlog::info("[quicksurf] system atoms={} grid={}x{}x{} sp={} backend={} "
                 "density={:.1f}ms mc={:.1f}ms tris={}",
                 atomCount, d[0], d[1], d[2], in.spacing, g_actualBackend,
                 ms(t0, t1), ms(t1, t2), mesh.indices.size() / 3);
    verts.resize(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        verts[i].pos = glm::vec4(mesh.positions[i], 0.0f);
        verts[i].normal = glm::vec4(mesh.normals[i], 0.0f);
        const glm::vec3 c = i < mesh.colors.size() ? mesh.colors[i]
                                                   : glm::vec3(0.55f, 0.75f, 0.95f);
        verts[i].color = glm::vec4(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
    }
    indices = std::move(mesh.indices);
    return !verts.empty() && !indices.empty();
}

// 写入整体系网格缓存（主线程与工作线程共用；内部加锁，调用方无需持锁）
void storeSystemQuickSurfMesh(uint64_t key, std::vector<TriMeshVertex> verts,
                              std::vector<uint32_t> indices) {
    auto entry = std::make_shared<CachedSurface>();
    entry->key = key;
    entry->verts = std::move(verts);
    entry->indices = std::move(indices);
    std::lock_guard<std::mutex> lk(g_systemMeshCache.mtx);
    entry->lastUse = ++g_systemMeshCache.tick;
    g_systemMeshCache.lastKey = key;
    g_systemMeshCache.entries[key] = std::move(entry);
    ++g_systemMeshCache.misses;
    // LRU 淘汰：只保留最近使用的 capacity 帧，避免长轨迹把内存吃满。
    while (g_systemMeshCache.entries.size() > g_systemMeshCache.capacity) {
        auto victim = g_systemMeshCache.entries.begin();
        for (auto it = g_systemMeshCache.entries.begin();
             it != g_systemMeshCache.entries.end(); ++it) {
            if (it->second->lastUse < victim->second->lastUse) {
                victim = it;
            }
        }
        if (victim->first == g_systemMeshCache.lastKey) {
            break;   // 不淘汰刚写入/正在使用的帧
        }
        g_systemMeshCache.entries.erase(victim);
    }
}

// —— 异步生成开关（见 quicksurf.h）——
void quickSurfSetAsyncEnabled(bool enabled) {
    g_asyncEnabled.store(enabled, std::memory_order_relaxed);
}

bool quickSurfAsyncEnabled() {
    return g_asyncEnabled.load(std::memory_order_relaxed);
}

void quickSurfSetFreezeGeometry(bool frozen) {
    g_freezeGeometry.store(frozen, std::memory_order_relaxed);
}

bool quickSurfFreezeGeometry() {
    return g_freezeGeometry.load(std::memory_order_relaxed);
}

// 组装一组世界坐标的体系表面输入（主线程）：可见性/选择掩码过滤 → 网格 → 指纹。
// pos/total 允许来自**未来帧**（见 prefetchSystemQuickSurf），不读写场景当前坐标。
bool buildSystemSurfaceInput(const RepContext& ctx, const glm::vec3* pos, size_t total,
                             SystemQuickSurfInput& in, uint64_t& fp) {
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
    std::vector<glm::vec3> colors;
    centers.reserve(total);
    radii.reserve(total);
    colors.reserve(total);
    const auto& insts = ctx.scene->instances();
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
            const auto& el = elementInfo(st.elements[i]);
            colors.emplace_back(el.color[0], el.color[1], el.color[2]);
        }
        globalBase += n;
    }
    if (centers.empty()) {
        return false;
    }

    glm::vec3 origin;
    glm::ivec3 dims;
    float sp = 0.0f;
    if (!computeGrid(centers, radii, 1.0f, ctx.rep->surfGridSpacing, origin, dims, sp)) {
        return false;
    }

    // 几何指纹：只含影响几何的输入；材质/透明度/配色改动不入键。
    fp = 1469598103934665603ull;
    auto mix = [&fp](const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            fp ^= p[i];
            fp *= 1099511628211ull;
        }
    };
    mix(&ctx.rep->surfGridSpacing, sizeof(float));
    mix(&ctx.rep->surfIsovalue, sizeof(float));
    mix(&ctx.rep->radiusScale, sizeof(float));
    mix(&dims, sizeof(dims));
    mix(&sp, sizeof(float));
    mix(centers.data(), centers.size() * sizeof(glm::vec3));
    mix(radii.data(), radii.size() * sizeof(float));
    mix(colors.data(), colors.size() * sizeof(glm::vec3));

    in.xyzr.resize(centers.size() * 4);
    in.flatColors.resize(colors.size() * 3);
    for (size_t i = 0; i < centers.size(); ++i) {
        in.xyzr[i * 4 + 0] = centers[i].x;
        in.xyzr[i * 4 + 1] = centers[i].y;
        in.xyzr[i * 4 + 2] = centers[i].z;
        in.xyzr[i * 4 + 3] = radii[i];
        in.flatColors[i * 3 + 0] = colors[i].x;
        in.flatColors[i * 3 + 1] = colors[i].y;
        in.flatColors[i * 3 + 2] = colors[i].z;
    }
    in.origin = origin;
    in.dims = dims;
    in.spacing = sp;
    in.isovalue = ctx.rep->surfIsovalue;
    return true;
}

// 向前预取后续帧的整体系表面（异步路径专用，主线程只做轻量组装+提交）。
// 播放到第 f 帧时，drawSystemQuickSurf 按同一指纹命中缓存 → 表面随轨迹逐帧正确
// 切换，而不再冻结/复用旧表面。窗口 6 帧：Release 单帧约 30–80 ms，后台算得过来。
void prefetchSystemQuickSurf(const RepContext& ctx, uint64_t currentFrame) {
    if (!quickSurfAsyncEnabled() || ctx.scene == nullptr || ctx.scene->instances().empty()) {
        return;
    }
    constexpr int kAhead = 6;
    // 没有多帧轨迹时不需要预取
    bool hasTrajectory = false;
    for (const auto& inst : ctx.scene->instances()) {
        if (inst.trajectory && inst.trajectory->frames.size() > 1) {
            hasTrajectory = true;
            break;
        }
    }
    if (!hasTrajectory) {
        return;
    }
    std::vector<glm::vec3> world;
    for (int d = 1; d <= kAhead; ++d) {
        const uint64_t frame = currentFrame + static_cast<uint64_t>(d);
        if (!ctx.scene->buildWorldPositionsForFrame(frame, world)) {
            continue;
        }
        SystemQuickSurfInput in;
        uint64_t fp = 0;
        if (!buildSystemSurfaceInput(ctx, world.data(), world.size(), in, fp)) {
            continue;
        }
        if (hasCachedSurface(fp)) {
            continue;   // 已算好/正在算
        }
        auto input = std::make_shared<SystemQuickSurfInput>(std::move(in));
        async::submitPrefetchSurfaceJob(fp, [input, fp]() {
            std::vector<TriMeshVertex> verts;
            std::vector<uint32_t> indices;
            if (buildSystemQuickSurfMesh(*input, verts, indices)) {
                storeSystemQuickSurfMesh(fp, std::move(verts), std::move(indices));
            }
        });
    }
}

// 体系级 QuickSurf（G5）：聚合全部可见实例原子（含溶剂分子）为一个整体高斯密度表面
// （而不是每分子一个独立小表面）。用于溶剂化大体系（如 md.gro）。
void drawSystemQuickSurf(const RepContext& ctx, SceneCommandList& out) {
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    const size_t total = ctx.frame && ctx.frame->positions ? ctx.frame->positions->size() : 0;
    SystemQuickSurfInput in;
    uint64_t fp = 0;
    if (!buildSystemSurfaceInput(ctx, pos, total, in, fp)) {
        return;
    }

    // —— 几何指纹 → 多帧网格缓存 ——
    // 命中（同一帧表面已由当前帧异步任务/后续帧预取算好）→ 直接发射。
    std::shared_ptr<CachedSurface> cached;
    {
        std::lock_guard<std::mutex> lk(g_systemMeshCache.mtx);
        cached = findCachedSurfaceLocked(fp);
        if (cached) {
            ++g_systemMeshCache.hits;
        }
    }
    if (cached) {
        emitQuickSurfMesh(cached->verts, cached->indices, out);
        prefetchSystemQuickSurf(ctx, ctx.frameIndex);
        return;
    }

    // 预取尚未追上（刚跳帧/刚开始播放）：冻结模式下先复用最近一张表面保证画面
    // 不断档，同时继续预取；下一帧命中缓存后即切成该帧的正确表面。
    if (quickSurfFreezeGeometry()) {
        std::shared_ptr<CachedSurface> fallback;
        {
            std::lock_guard<std::mutex> lk(g_systemMeshCache.mtx);
            if (!g_systemMeshCache.entries.empty()) {
                auto it = g_systemMeshCache.entries.find(g_systemMeshCache.lastKey);
                if (it == g_systemMeshCache.entries.end()) {
                    it = g_systemMeshCache.entries.begin();
                }
                for (auto cur = g_systemMeshCache.entries.begin();
                     cur != g_systemMeshCache.entries.end(); ++cur) {
                    if (cur->second->lastUse > it->second->lastUse) {
                        it = cur;
                    }
                }
                fallback = it->second;
                fallback->lastUse = ++g_systemMeshCache.tick;
                ++g_systemMeshCache.hits;
            }
        }
        if (fallback) {
            emitQuickSurfMesh(fallback->verts, fallback->indices, out);
            prefetchSystemQuickSurf(ctx, ctx.frameIndex);
            return;
        }
    }

    // —— 异步路径（GUI 主线程）——
    // 缓存未命中 → 提交后台生成并立即返回：本帧不发射该表面，Renderer 检测到
    // "有任务在跑"后保留旧命令流（旧画面继续显示）；任务完成 → 主线程下一次重建
    // 时命中上面的缓存 → 表面一次换上。主线程从此不再阻塞 ~197 ms。
    if (quickSurfAsyncEnabled()) {
        auto input = std::make_shared<SystemQuickSurfInput>(std::move(in));
        async::submitSurfaceJob(fp, [input, fp]() {
            std::vector<TriMeshVertex> verts;
            std::vector<uint32_t> indices;
            if (buildSystemQuickSurfMesh(*input, verts, indices)) {
                storeSystemQuickSurfMesh(fp, std::move(verts), std::move(indices));
            }
        });
        prefetchSystemQuickSurf(ctx, ctx.frameIndex);
        return;
    }

    // —— 同步路径（CLI / 导出线程 / 单元测试 / Renderer 关闭异步时）——
    std::vector<TriMeshVertex> verts;
    std::vector<uint32_t> indices;
    if (!buildSystemQuickSurfMesh(in, verts, indices)) {
        return;
    }
    // 写入缓存（供材质/配色等**不影响几何**的改动命中，省掉整次密度场 + MC）
    storeSystemQuickSurfMesh(fp, verts, indices);
    emitQuickSurfMesh(verts, indices, out);
}

} // namespace moleculens::engine
