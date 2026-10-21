// =============================================================================
// MolecuLens · engine · style/quicksurf/quicksurf_avx2.cpp —— QuickSurf AVX2 密度核
// 规格来源：QS.2 参照 QuickSurf_AVX2.C（8 宽 SIMD + 快速指数）
// 本 TU 以 /arch:AVX2 单独编译（其它源不受影响）。未以 AVX2 编译时退化为空实现。
// 运行时经 quicksurf.cpp 的 dispatch 检测 CPUID → 选择本内核或标量多线程。
// 说明：密度指数用 Schraudolph 位操作快速近似（与标量 std::exp 略有差异，可忽略）。
//
// 并行：按 z 轴切片（每线程负责一段 z 层），**不做归约**——每个体素仍然按
// 原子序顺序累加，所以结果与单线程版本逐位一致。这样大网格能吃满多核，
// 又不额外分配 natoms×nvox 的线程私有网格（100³×4 float×16 线程 = 256 MB）。
// 小网格（每线程 < 64K 体素）直接单线程：2042 个水分子各 18³ 的场景里，
// 每次调用都创建 16 个线程反而更慢。
// =============================================================================

#include "engine/style/quicksurf/quicksurf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#include <intrin.h>
#endif

namespace moleculens::engine {

bool avx2KernelAvailable() {
#if defined(__AVX2__)
    // CPUID.7.0.EBX.AVX2 + OSXSAVE + XGETBV(XMM|YMM)
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] < 7) {
        return false;
    }
    __cpuidex(cpuInfo, 7, 0);
    if (!(cpuInfo[1] & (1 << 5))) {
        return false;
    }
    __cpuid(cpuInfo, 1);
    if (!(cpuInfo[2] & (1 << 27))) {
        return false;
    }
    const uint64_t xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) {
        return false;
    }
    return true;
#else
    return false;
#endif
}

#if defined(__AVX2__)
namespace {

// 处理原子区间 [a0,a1)、z 层区间 [zBegin,zEnd) 的密度/梯度/颜色累加。
void densityAtomSliceAvx2(const float* xyzr, const float* colors, int a0, int a1,
                          float radscale, float gridspacing, float gausslim,
                          const float origin[3], const int dims[3], int zBegin,
                          int zEnd, float* density, float* grad, float* colormap) {
    const int nx = dims[0], ny = dims[1], nz = dims[2];
    const float invgs = 1.0f / gridspacing;

    // 快速 e^x（x<=0）：Schraudolph 位操作近似
    const __m256 kExpA = _mm256_set1_ps(12102203.2f);   // 2^23 / ln2
    const __m256 kExpB = _mm256_set1_ps(1065353216.0f); // 127 * 2^23
    const auto fastExp = [&](__m256 x) {
        const __m256 y = _mm256_add_ps(_mm256_mul_ps(x, kExpA), kExpB);
        return _mm256_castsi256_ps(_mm256_cvtps_epi32(y));
    };

    const __m256 kGrid = _mm256_set1_ps(gridspacing);
    const __m256 kNegOne = _mm256_set1_ps(-1.0f);
    const __m256i kIdx = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256 kIdxF = _mm256_cvtepi32_ps(kIdx);

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
        int az0 = std::max(0, static_cast<int>(std::floor((cz - radlim - origin[2]) * invgs)));
        int az1 = std::min(nz - 1, static_cast<int>(std::ceil((cz + radlim - origin[2]) * invgs)));
        // 与本线程的 z 切片求交
        az0 = std::max(az0, zBegin);
        az1 = std::min(az1, zEnd - 1);
        if (az0 > az1) {
            continue;
        }

        const __m256 kArinv = _mm256_set1_ps(arinv);
        const __m256 kNegArinv = _mm256_set1_ps(-arinv);
        const __m256 kCx = _mm256_set1_ps(cx);
        const __m256 kCy = _mm256_set1_ps(cy);
        const __m256 kCz = _mm256_set1_ps(cz);

        for (int z = az0; z <= az1; ++z) {
            const float dz = origin[2] + static_cast<float>(z) * gridspacing - cz;
            const float oz = origin[2] + static_cast<float>(z) * gridspacing;
            const size_t zbase = static_cast<size_t>(z) * nx * ny;
            for (int y = y0; y <= y1; ++y) {
                const float dy = origin[1] + static_cast<float>(y) * gridspacing - cy;
                const float dy2dz2 = dy * dy + dz * dz;
                if (dy2dz2 >= radlim2) {
                    continue;
                }
                const float oy = origin[1] + static_cast<float>(y) * gridspacing;
                const size_t base = zbase + static_cast<size_t>(y) * nx;
                const __m256 kDy2dz2 = _mm256_set1_ps(dy2dz2);
                const __m256 kOy = _mm256_set1_ps(oy);
                const __m256 kOz = _mm256_set1_ps(oz);
                // 8 宽主循环（x..x+7）
                int x = x0;
                for (; x + 7 <= x1; x += 8) {
                    const float xBase = origin[0] + static_cast<float>(x) * gridspacing - cx;
                    const __m256 dx = _mm256_add_ps(_mm256_set1_ps(xBase),
                                                    _mm256_mul_ps(kIdxF, kGrid));
                    const __m256 r2 = _mm256_add_ps(_mm256_mul_ps(dx, dx), kDy2dz2);
                    const __m256 gv = fastExp(_mm256_mul_ps(r2, kNegArinv));
                    float g[8];
                    _mm256_storeu_ps(g, gv);
                    for (int j = 0; j < 8; ++j) {
                        const size_t addr = static_cast<size_t>(base + x + j);
                        density[addr] += g[j];
                        const float pcx = origin[0] + static_cast<float>(x + j) * gridspacing;
                        grad[3 * addr + 0] += 2.0f * arinv * (cx - pcx) * g[j];
                        grad[3 * addr + 1] += 2.0f * arinv * (cy - oy) * g[j];
                        grad[3 * addr + 2] += 2.0f * arinv * (cz - oz) * g[j];
                        if (colormap) {
                            colormap[3 * addr + 0] += g[j] * acr;
                            colormap[3 * addr + 1] += g[j] * acg;
                            colormap[3 * addr + 2] += g[j] * acb;
                        }
                    }
                }
                // 尾部标量
                for (; x <= x1; ++x) {
                    const float pcx = origin[0] + static_cast<float>(x) * gridspacing;
                    const float r2 = (pcx - cx) * (pcx - cx) + dy2dz2;
                    const float g = std::exp(-r2 * arinv);
                    const size_t addr = base + static_cast<size_t>(x);
                    density[addr] += g;
                    grad[3 * addr + 0] += 2.0f * arinv * (cx - pcx) * g;
                    grad[3 * addr + 1] += 2.0f * arinv * (cy - oy) * g;
                    grad[3 * addr + 2] += 2.0f * arinv * (cz - oz) * g;
                    if (colormap) {
                        colormap[3 * addr + 0] += g * acr;
                        colormap[3 * addr + 1] += g * acg;
                        colormap[3 * addr + 2] += g * acb;
                    }
                }
            }
        }
    }
}

} // namespace
#endif

void computeDensityAvx2(const float* xyzr, int natoms, const float* colors,
                        int a0, int a1, float radscale, float gridspacing,
                        float gausslim, const float origin[3], const int dims[3],
                        float* density, float* grad, float* colormap) {
    (void)natoms;
#if defined(__AVX2__)
    const size_t nvox = static_cast<size_t>(dims[0]) * dims[1] * dims[2];
    const int nz = dims[2];
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    // MOLECULENS_QUICKSURF_THREADS=N：限线程数（基准测试 / 低功耗场景）
    if (const char* tv = std::getenv("MOLECULENS_QUICKSURF_THREADS")) {
        const int n = std::atoi(tv);
        if (n > 0) {
            hw = std::min(hw, static_cast<unsigned>(n));
        }
    }
    // 每线程至少 64K 体素才值得开线程（小分子场景下线程创建比计算还贵）
    int nthreads = static_cast<int>(std::min<size_t>(hw, std::max<size_t>(1, nvox / 65536)));
    if (nthreads <= 1 || nz < 2) {
        densityAtomSliceAvx2(xyzr, colors, a0, a1, radscale, gridspacing, gausslim,
                             origin, dims, 0, nz, density, grad, colormap);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads));
    for (int t = 0; t < nthreads; ++t) {
        const int z0 = nz * t / nthreads;
        const int z1 = nz * (t + 1) / nthreads;
        pool.emplace_back([=]() {
            densityAtomSliceAvx2(xyzr, colors, a0, a1, radscale, gridspacing, gausslim,
                                 origin, dims, z0, z1, density, grad, colormap);
        });
    }
    for (auto& th : pool) {
        th.join();
    }
#else
    (void)xyzr; (void)colors; (void)a0; (void)a1;
    (void)radscale; (void)gridspacing; (void)gausslim;
    (void)origin; (void)dims; (void)density; (void)grad; (void)colormap;
#endif
}

} // namespace moleculens::engine
