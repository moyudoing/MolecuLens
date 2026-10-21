// =============================================================================
// MolecuLens · engine · style/espsurface/esp_surface.cpp —— ESP（静电势）映射表面实现
// 规格来源：_esp/DESIGN.md；契约头 style/espsurface/esp_surface.h（公开签名固定）。
// 数据流：density cube →（轴间距 >0.15 Å 时三线性重采样到 ~0.12 Å 细网格）
//         → Marching Cubes 抽取 ρ == isolevel 等值面（顶点按 1e-4 Å 量化合并）
//         → Taubin 平滑（λ=0.5 / μ=-0.53 × 3 次，防收缩）
//         → 平滑后用细网格密度梯度重算外向法线
//         → ESP cube 逐顶点三线性采样 ×627.509 → kcal/mol
//         → 7 种标准色标（默认 RWB）逐顶点 RGB → TriMesh（逐顶点色，meshBlend）
//         → 唯一顶点 1e-4 Å 去重 + 1.2 Å 空间哈希 → 局部极值（色标条/数值标注）。
// 缓存：两 cube 路径 + 全部参数 + 两文件 size/mtime 为键（LRU 最多 4 条）。命令流在
//       材质/透明度等改动时会整体重建，等值面抽取/平滑/采样无需重算。
// 线程安全：static 缓存与 overlay 均由互斥量保护（主线程与 PNG 导出线程会并发进入）。
// =============================================================================

#include "engine/style/espsurface/esp_surface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/matrix.hpp>

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"
#include "engine/style/surface/mc_tables.h"
#include "io/cube/cube_volume.h"

namespace moleculens::engine {

namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// 缓存条目上限（同时可能有两份 cube 参数集在视口中切换，4 条足够）
constexpr std::size_t kMaxCacheEntries = 4;
// 极值点邻域/非极大抑制半径（Å）
constexpr float kExtremumRadius = 1.2f;
constexpr float kExtremumRadius2 = kExtremumRadius * kExtremumRadius;
// 顶点去重量化尺度：1e-4 Å → 乘 1e4 后取整
constexpr float kExtremumQuant = 1e4f;
constexpr float kMergeQuant = 1e4f;   // 唯一顶点合并量化（1e-4 Å）
// 每类极值点最多保留数量
constexpr std::size_t kMaxExtremaPerType = 8;
// 向量退化判定阈值（平方长度）
constexpr float kTinyLength2 = 1e-20f;

// —— 画质提升：细网格重采样 + Taubin 平滑 ——
// 原 cube 轴间距大于该值时重采样（demo 0.25 Å 会重采样，16³@0.4 Å 单测也会）
constexpr double kResampleThreshold = 0.15;      // Å
// 细网格目标间距（原 demo 0.25 Å → 0.12 Å，台阶/棱角显著减少）
constexpr double kResampleTargetSpacing = 0.12;  // Å
// 细网格上限（防爆内存/耗时）：单轴 ≤ 420、总体素 ≤ 16M
constexpr int    kFineMaxDim = 420;
constexpr std::size_t kFineMaxVoxels = 16u * 1024u * 1024u;
// Taubin 平滑参数（λ>0 低通，μ<0 抗收缩；3 次迭代）
constexpr int   kTaubinIterations = 3;
constexpr float kTaubinLambda = 0.5f;
constexpr float kTaubinMu = -0.53f;

// ---------------------------------------------------------------------------
// 色标（标准 matplotlib 控制点；首尾覆盖 t=0/1）
// ---------------------------------------------------------------------------

struct RgbColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct ColorStop {
    float    t = 0.0f;
    RgbColor color{};
};

// RWB（默认，化学惯例）：t=0 纯红（富电子/负电势）→ 0.5 白 → 1 纯蓝（正电势）
constexpr ColorStop kRwbStops[] = {
    {0.0f, {1.0f, 0.0f, 0.0f}},
    {0.5f, {1.0f, 1.0f, 1.0f}},
    {1.0f, {0.0f, 0.0f, 1.0f}},
};

// BWR：与 RWB 反相
constexpr ColorStop kBwrStops[] = {
    {0.0f, {0.0f, 0.0f, 1.0f}},
    {0.5f, {1.0f, 1.0f, 1.0f}},
    {1.0f, {1.0f, 0.0f, 0.0f}},
};

// Coolwarm（matplotlib coolwarm 端点 + 中间锚点：蓝 → 浅灰 → 红）
constexpr ColorStop kCoolwarmStops[] = {
    {0.00f, {0.229806f, 0.298718f, 0.753683f}},
    {0.25f, {0.554324f, 0.690440f, 0.995580f}},
    {0.50f, {0.865435f, 0.865435f, 0.865435f}},
    {0.75f, {0.957429f, 0.597624f, 0.462021f}},
    {1.00f, {0.705673f, 0.015556f, 0.150233f}},
};

// Turbo（Google Turbo 标准多项式在 9 个 t 上的取值，sRGB）
constexpr ColorStop kTurboStops[] = {
    {0.000f, {0.135721f, 0.091403f, 0.106673f}},
    {0.125f, {0.268616f, 0.414773f, 0.934760f}},
    {0.250f, {0.148313f, 0.740465f, 0.880723f}},
    {0.375f, {0.250394f, 0.952795f, 0.572903f}},
    {0.500f, {0.588522f, 0.981864f, 0.313169f}},
    {0.625f, {0.931551f, 0.813924f, 0.177069f}},
    {0.750f, {1.000000f, 0.501735f, 0.113986f}},
    {0.875f, {0.786373f, 0.174933f, 0.047287f}},
    {1.000f, {0.565859f, 0.050389f, 0.000000f}},
};

// Viridis（matplotlib 标准 10 锚点，十六进制 sRGB 直转 0..1 float）：
// #440154 #482878 #3e4989 #31688e #26828e #1f9e89 #35b779 #6ece58 #b5de2b #fde725
constexpr ColorStop kViridisStops[] = {
    {0.000000f, {0.266667f, 0.003922f, 0.329412f}},   // #440154
    {0.111111f, {0.282353f, 0.156863f, 0.470588f}},   // #482878
    {0.222222f, {0.243137f, 0.286275f, 0.537255f}},   // #3e4989
    {0.333333f, {0.192157f, 0.407843f, 0.556863f}},   // #31688e
    {0.444444f, {0.149020f, 0.509804f, 0.556863f}},   // #26828e
    {0.555556f, {0.121569f, 0.619608f, 0.537255f}},   // #1f9e89
    {0.666667f, {0.207843f, 0.717647f, 0.474510f}},   // #35b779
    {0.777778f, {0.431373f, 0.807843f, 0.345098f}},   // #6ece58
    {0.888889f, {0.709804f, 0.870588f, 0.168627f}},   // #b5de2b
    {1.000000f, {0.992157f, 0.905882f, 0.145098f}},   // #fde725
};

// Reference（参考截图配色）：从用户提供的 GXNU MolStudio ESP 色标条实测采样
// （低值黄 → 绿 → 青 → 蓝 → 紫 = 高值；正电势紫、负电势黄，与参考图一致）。
constexpr ColorStop kReferenceStops[] = {
    {0.0000f, {0.8941f, 0.8667f, 0.2275f}},
    {0.1000f, {0.7020f, 0.8588f, 0.2118f}},
    {0.2000f, {0.4824f, 0.7765f, 0.3686f}},
    {0.3000f, {0.2627f, 0.7373f, 0.4588f}},
    {0.4000f, {0.1490f, 0.6039f, 0.4902f}},
    {0.5000f, {0.1451f, 0.5608f, 0.5412f}},
    {0.6000f, {0.2000f, 0.4549f, 0.5255f}},
    {0.7000f, {0.2235f, 0.3725f, 0.5412f}},
    {0.8000f, {0.2471f, 0.2706f, 0.5059f}},
    {0.9000f, {0.2784f, 0.1647f, 0.4510f}},
    {1.0000f, {0.2235f, 0.0549f, 0.3333f}},
};

// Jet（经典蓝 → 青 → 绿 → 黄 → 红；两端深色，与标准 jet 分段一致）
constexpr ColorStop kJetStops[] = {
    {0.000f, {0.0f, 0.0f, 0.5f}},
    {0.125f, {0.0f, 0.0f, 1.0f}},
    {0.375f, {0.0f, 1.0f, 1.0f}},
    {0.625f, {1.0f, 1.0f, 0.0f}},
    {0.875f, {1.0f, 0.0f, 0.0f}},
    {1.000f, {0.5f, 0.0f, 0.0f}},
};

template <typename T, std::size_t N>
constexpr std::size_t arrayCount(const T (&)[N]) noexcept {
    return N;
}

struct StopTable {
    const ColorStop* data = nullptr;
    std::size_t      count = 0;
};

StopTable stopsFor(EspColormap colormap) noexcept {
    switch (colormap) {
        case EspColormap::Rwb:
            return {kRwbStops, arrayCount(kRwbStops)};
        case EspColormap::Bwr:
            return {kBwrStops, arrayCount(kBwrStops)};
        case EspColormap::Coolwarm:
            return {kCoolwarmStops, arrayCount(kCoolwarmStops)};
        case EspColormap::Turbo:
            return {kTurboStops, arrayCount(kTurboStops)};
        case EspColormap::Viridis:
            return {kViridisStops, arrayCount(kViridisStops)};
        case EspColormap::Jet:
            return {kJetStops, arrayCount(kJetStops)};
        case EspColormap::Reference:
            return {kReferenceStops, arrayCount(kReferenceStops)};
        case EspColormap::Count:
        default:
            return {kRwbStops, arrayCount(kRwbStops)};
    }
}

// 发散型色标（自动范围取 ±99.5 分位 |v|）；顺序型取 [1,99] 分位
bool isDivergentColormap(EspColormap colormap) noexcept {
    switch (colormap) {
        case EspColormap::Rwb:
        case EspColormap::Bwr:
        case EspColormap::Coolwarm:
            return true;
        default:
            return false;
    }
}

// Representation::espColormap 是 int32_t（representation.h 不反向依赖本头），这里统一净化
EspColormap sanitizeColormap(unsigned raw) noexcept {
    return raw < static_cast<unsigned>(EspColormap::Count)
               ? static_cast<EspColormap>(raw)
               : EspColormap::Rwb;
}

// ---------------------------------------------------------------------------
// 文件指纹 / 表面网格缓存
// ---------------------------------------------------------------------------

struct FileFingerprint {
    uint64_t size = 0;
    int64_t  mtime = 0;

    bool operator==(const FileFingerprint& other) const noexcept {
        return size == other.size && mtime == other.mtime;
    }
};

FileFingerprint fingerprintOf(const std::string& path) {
    FileFingerprint fingerprint;
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (!ec) {
        fingerprint.size = static_cast<uint64_t>(bytes);
    }
    ec.clear();
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        fingerprint.mtime = static_cast<int64_t>(writeTime.time_since_epoch().count());
    }
    return fingerprint;
}

struct BuildKey {
    std::string     densityPath;
    std::string     potentialPath;
    float           isolevel = 0.0f;
    float           vminKcal = 0.0f;
    float           vmaxKcal = 0.0f;
    bool            autoRange = false;
    bool            invert = false;
    EspColormap     colormap = EspColormap::Rwb;
    FileFingerprint densityFile;
    FileFingerprint potentialFile;
};

bool operator==(const BuildKey& a, const BuildKey& b) noexcept {
    return a.densityPath == b.densityPath && a.potentialPath == b.potentialPath &&
           a.isolevel == b.isolevel && a.vminKcal == b.vminKcal &&
           a.vmaxKcal == b.vmaxKcal && a.autoRange == b.autoRange &&
           a.invert == b.invert && a.colormap == b.colormap &&
           a.densityFile == b.densityFile && a.potentialFile == b.potentialFile;
}

struct CacheEntry {
    BuildKey       key;
    EspSurfaceMesh mesh;
    uint64_t       lastUse = 0;
};

std::mutex              g_cacheMutex;
std::vector<CacheEntry> g_cache;
uint64_t                g_cacheClock = 0;
// 构建串行化：主线程与 PNG 导出线程可能同时 miss 同一键 —— 只让一个线程真正
// 解析/抽取/平滑，另一个等它完成后直接命中缓存（避免重复 CPU 工作与解析器并发风险）。
std::mutex              g_buildMutex;

bool cacheLookup(const BuildKey& key, EspSurfaceMesh& out) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    for (CacheEntry& entry : g_cache) {
        if (entry.key == key) {
            entry.lastUse = ++g_cacheClock;
            out = entry.mesh;
            return true;
        }
    }
    return false;
}

void cacheStore(const BuildKey& key, const EspSurfaceMesh& mesh) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    for (CacheEntry& entry : g_cache) {
        if (entry.key == key) {
            entry.mesh = mesh;
            entry.lastUse = ++g_cacheClock;
            return;
        }
    }
    if (g_cache.size() >= kMaxCacheEntries) {
        auto oldest = std::min_element(g_cache.begin(), g_cache.end(),
                                       [](const CacheEntry& a, const CacheEntry& b) {
                                           return a.lastUse < b.lastUse;
                                       });
        if (oldest != g_cache.end()) {
            g_cache.erase(oldest);
        }
    }
    CacheEntry entry;
    entry.key = key;
    entry.mesh = mesh;
    entry.lastUse = ++g_cacheClock;
    g_cache.push_back(std::move(entry));
}

// ---------------------------------------------------------------------------
// 视口叠加信息（色标条 + 极值标签）全局状态
// ---------------------------------------------------------------------------

std::mutex g_overlayMutex;
EspOverlay g_overlay;

// ---------------------------------------------------------------------------
// 基础几何 / 采样辅助
// ---------------------------------------------------------------------------

// 世界坐标 → 分数体素坐标（world = origin + A·f，A 的列为三条轴向量）
bool computeWorldToFractional(const entity::CubeVolume& cube, glm::dmat3& out) {
    const glm::dmat3 axes(glm::dvec3(cube.axes[0]), glm::dvec3(cube.axes[1]),
                          glm::dvec3(cube.axes[2]));
    const double det = glm::determinant(axes);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) {
        return false;
    }
    out = glm::inverse(axes);
    return true;
}

uint64_t mixHash(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint64_t positionHash(int64_t x, int64_t y, int64_t z) noexcept {
    uint64_t hash = mixHash(static_cast<uint64_t>(x));
    hash ^= mixHash(static_cast<uint64_t>(y) + 0x9E3779B97F4A7C15ULL);
    hash ^= mixHash(static_cast<uint64_t>(z) + 0xC2B2AE3D27D4EB4FULL);
    return hash;
}

// 分数体素坐标三线性采样（坐标钳制到网格内；越界顶点用边界值，避免表面边缘异常）
float sampleTrilinearClamped(const entity::CubeVolume& cube, const glm::dvec3& fractional) {
    const int nx = cube.dims[0];
    const int ny = cube.dims[1];
    const int nz = cube.dims[2];
    if (nx < 2 || ny < 2 || nz < 2) {
        return 0.0f;
    }
    const double x = std::min(std::max(fractional.x, 0.0), static_cast<double>(nx - 1));
    const double y = std::min(std::max(fractional.y, 0.0), static_cast<double>(ny - 1));
    const double z = std::min(std::max(fractional.z, 0.0), static_cast<double>(nz - 1));
    const int x0 = std::min(static_cast<int>(std::floor(x)), nx - 2);
    const int y0 = std::min(static_cast<int>(std::floor(y)), ny - 2);
    const int z0 = std::min(static_cast<int>(std::floor(z)), nz - 2);
    const float fx = static_cast<float>(x - static_cast<double>(x0));
    const float fy = static_cast<float>(y - static_cast<double>(y0));
    const float fz = static_cast<float>(z - static_cast<double>(z0));

    const float* data = cube.data.data();
    const std::size_t strideX = static_cast<std::size_t>(ny) * nz;
    const std::size_t strideY = static_cast<std::size_t>(nz);
    const float* base = data + static_cast<std::size_t>(x0) * strideX +
                        static_cast<std::size_t>(y0) * strideY + static_cast<std::size_t>(z0);
    const float c000 = base[0];
    const float c001 = base[1];
    const float c010 = base[strideY];
    const float c011 = base[strideY + 1];
    const float c100 = base[strideX];
    const float c101 = base[strideX + 1];
    const float c110 = base[strideX + strideY];
    const float c111 = base[strideX + strideY + 1];

    const float c00 = c000 + (c100 - c000) * fx;
    const float c01 = c001 + (c101 - c001) * fx;
    const float c10 = c010 + (c110 - c010) * fx;
    const float c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}

// 顶点处 ESP 采样 → kcal/mol（esp cube 未覆盖到顶点时按钳制坐标外推边界值）
float samplePotentialKcal(const entity::CubeVolume& potential, const glm::dmat3* worldToFractional,
                          const glm::vec3& world) {
    float raw = 0.0f;
    if (potential.sampleTrilinear(world, raw)) {
        return std::isfinite(raw) ? raw * kEspAuToKcal : 0.0f;
    }
    if (worldToFractional == nullptr) {
        return 0.0f;
    }
    const glm::dvec3 fractional =
        (*worldToFractional) * (glm::dvec3(world) - glm::dvec3(potential.origin));
    const float clamped = sampleTrilinearClamped(potential, fractional);
    return std::isfinite(clamped) ? clamped * kEspAuToKcal : 0.0f;
}

// 质心（用于法线朝外校正）：优先用 cube 原子记录，否则取密度网格包围盒中心
glm::vec3 computeCenterOfMass(const entity::CubeVolume& density) {
    if (!density.atoms.empty()) {
        glm::dvec3 sum(0.0);
        for (const entity::CubeAtom& atom : density.atoms) {
            sum += glm::dvec3(atom.position);
        }
        return glm::vec3(sum / static_cast<double>(density.atoms.size()));
    }
    const glm::vec3 centerIndex(static_cast<float>(density.dims[0] - 1) * 0.5f,
                                static_cast<float>(density.dims[1] - 1) * 0.5f,
                                static_cast<float>(density.dims[2] - 1) * 0.5f);
    return density.origin + density.axes[0] * centerIndex.x + density.axes[1] * centerIndex.y +
           density.axes[2] * centerIndex.z;
}

// 径向方向（单位向量）；退化时给 +Z
glm::vec3 radialDirection(const glm::vec3& point, const glm::vec3& center) {
    const glm::vec3 delta = point - center;
    const float length2 = glm::dot(delta, delta);
    if (length2 > kTinyLength2) {
        return delta / std::sqrt(length2);
    }
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

// 密度数据范围（忽略非有限值）；空/全非法返回 false
bool computeDataRange(const entity::CubeVolume& cube, float& outMin, float& outMax) {
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    bool any = false;
    for (float value : cube.data) {
        if (!std::isfinite(value)) {
            continue;
        }
        lo = std::min(lo, value);
        hi = std::max(hi, value);
        any = true;
    }
    outMin = lo;
    outMax = hi;
    return any;
}

// ---------------------------------------------------------------------------
// 细网格重采样（density 轴间距 > 0.15 Å 时）
// ---------------------------------------------------------------------------

// 三线性重采样：把 source 密度场写进按 (ix*ny+iy)*nz+iz 布局的目标数组。
// fineToSourceFractional = A_source^{-1} · A_fine（细索引 → 原分数坐标，原点相同）。
void resampleDensityTrilinear(const entity::CubeVolume& source,
                              const std::array<int, 3>& targetDims,
                              const glm::dmat3& fineToSourceFractional,
                              std::vector<float>& destination) {
    const int nx = targetDims[0];
    const int ny = targetDims[1];
    const int nz = targetDims[2];
    const int sourceNx = source.dims[0];
    const int sourceNy = source.dims[1];
    const int sourceNz = source.dims[2];
    const float* sourceData = source.data.data();
    float* destinationData = destination.data();

    const std::size_t strideX = static_cast<std::size_t>(sourceNy) * sourceNz;
    const std::size_t strideY = static_cast<std::size_t>(sourceNz);
    const glm::dvec3 column0 = fineToSourceFractional[0];
    const glm::dvec3 column1 = fineToSourceFractional[1];
    const glm::dvec3 column2 = fineToSourceFractional[2];
    const double maxFractionX = static_cast<double>(sourceNx - 1);
    const double maxFractionY = static_cast<double>(sourceNy - 1);
    const double maxFractionZ = static_cast<double>(sourceNz - 1);

    // 内层用标量增量（而非 dvec3 运算）：Debug 构建下可省掉大量运算符函数调用
    for (int ix = 0; ix < nx; ++ix) {
        const double baseX = column0.x * static_cast<double>(ix);
        const double baseY = column0.y * static_cast<double>(ix);
        const double baseZ = column0.z * static_cast<double>(ix);
        for (int iy = 0; iy < ny; ++iy) {
            double fx = baseX + column1.x * static_cast<double>(iy);
            double fy = baseY + column1.y * static_cast<double>(iy);
            double fz = baseZ + column1.z * static_cast<double>(iy);
            for (int iz = 0; iz < nz; ++iz) {
                // 端点对齐保证落界内；钳制仅防御浮点误差
                if (fx < 0.0) {
                    fx = 0.0;
                } else if (fx > maxFractionX) {
                    fx = maxFractionX;
                }
                if (fy < 0.0) {
                    fy = 0.0;
                } else if (fy > maxFractionY) {
                    fy = maxFractionY;
                }
                if (fz < 0.0) {
                    fz = 0.0;
                } else if (fz > maxFractionZ) {
                    fz = maxFractionZ;
                }
                int x0 = static_cast<int>(fx);
                int y0 = static_cast<int>(fy);
                int z0 = static_cast<int>(fz);
                if (x0 > sourceNx - 2) {
                    x0 = sourceNx - 2;
                }
                if (y0 > sourceNy - 2) {
                    y0 = sourceNy - 2;
                }
                if (z0 > sourceNz - 2) {
                    z0 = sourceNz - 2;
                }
                const double tx = fx - static_cast<double>(x0);
                const double ty = fy - static_cast<double>(y0);
                const double tz = fz - static_cast<double>(z0);

                const float* base = sourceData + static_cast<std::size_t>(x0) * strideX +
                                    static_cast<std::size_t>(y0) * strideY +
                                    static_cast<std::size_t>(z0);
                const double c000 = base[0];
                const double c001 = base[1];
                const double c010 = base[strideY];
                const double c011 = base[strideY + 1];
                const double c100 = base[strideX];
                const double c101 = base[strideX + 1];
                const double c110 = base[strideX + strideY];
                const double c111 = base[strideX + strideY + 1];
                const double c00 = c000 + (c100 - c000) * tx;
                const double c01 = c001 + (c101 - c001) * tx;
                const double c10 = c010 + (c110 - c010) * tx;
                const double c11 = c011 + (c111 - c011) * tx;
                const double c0 = c00 + (c10 - c00) * ty;
                const double c1 = c01 + (c11 - c01) * ty;
                *destinationData++ = static_cast<float>(c0 + (c1 - c0) * tz);
                fx += column2.x;
                fy += column2.y;
                fz += column2.z;
            }
        }
    }
}

// 构造 ~0.12 Å 细网格（复制 origin/原子/种类，重算 axesInverse）。
// 返回 false 表示不需要重采样或无法重采样（调用方退回原网格）。
bool buildWorkingDensity(const entity::CubeVolume& source, entity::CubeVolume& out) {
    if (source.dims[0] < 2 || source.dims[1] < 2 || source.dims[2] < 2) {
        return false;
    }
    glm::dvec3 stepVector[3];
    glm::dvec3 extentVector[3];
    double step[3] = {0.0, 0.0, 0.0};
    double extent[3] = {0.0, 0.0, 0.0};
    for (int axis = 0; axis < 3; ++axis) {
        stepVector[axis] = glm::dvec3(source.axes[axis]);
        step[axis] = glm::length(stepVector[axis]);
        extentVector[axis] = stepVector[axis] * static_cast<double>(source.dims[axis] - 1);
        extent[axis] = glm::length(extentVector[axis]);
    }
    const double maxStep = std::max({step[0], step[1], step[2]});
    if (maxStep <= kResampleThreshold) {
        return false;   // 原网格已足够细
    }
    if (!(extent[0] > 1e-9 && extent[1] > 1e-9 && extent[2] > 1e-9)) {
        return false;   // 退化网格：不重采样
    }

    // 计算细网格维度：目标 ~0.12 Å，单轴 ≤ 420、总体素 ≤ 16M（超限则放大间距）
    double spacing = kResampleTargetSpacing;
    int dims[3] = {2, 2, 2};
    bool resolved = false;
    for (int attempt = 0; attempt < 12 && !resolved; ++attempt) {
        int nextDims[3] = {2, 2, 2};
        bool adjusted = false;
        for (int axis = 0; axis < 3; ++axis) {
            const double count = extent[axis] / spacing;
            if (count > static_cast<double>(kFineMaxDim - 1)) {
                spacing = std::max(spacing,
                                   extent[axis] / static_cast<double>(kFineMaxDim - 1));
                adjusted = true;
            }
        }
        if (adjusted) {
            continue;
        }
        for (int axis = 0; axis < 3; ++axis) {
            const double count = extent[axis] / spacing;
            nextDims[axis] = std::max(2, static_cast<int>(std::llround(count)) + 1);
        }
        const std::size_t total = static_cast<std::size_t>(nextDims[0]) *
                                  static_cast<std::size_t>(nextDims[1]) *
                                  static_cast<std::size_t>(nextDims[2]);
        if (total > kFineMaxVoxels) {
            spacing *= std::cbrt(static_cast<double>(total) / static_cast<double>(kFineMaxVoxels));
            continue;
        }
        for (int axis = 0; axis < 3; ++axis) {
            dims[axis] = nextDims[axis];
        }
        resolved = true;
    }
    if (!resolved) {
        return false;
    }

    // 细网格轴向量：保持端点跨度不变（axes_fine[i] = extent_i / (dims_i - 1)）
    glm::dmat3 fineAxes(1.0);
    glm::dmat3 sourceAxes(1.0);
    for (int axis = 0; axis < 3; ++axis) {
        sourceAxes[axis] = stepVector[axis];
        const double count = static_cast<double>(dims[axis] - 1);
        fineAxes[axis] = extentVector[axis] / count;
    }
    const double sourceDet = glm::determinant(sourceAxes);
    const double fineDet = glm::determinant(fineAxes);
    if (!std::isfinite(sourceDet) || std::abs(sourceDet) < 1e-12 ||
        !std::isfinite(fineDet) || std::abs(fineDet) < 1e-12) {
        return false;
    }

    entity::CubeVolume fine;
    fine.dims = {dims[0], dims[1], dims[2]};
    fine.origin = source.origin;
    fine.atoms = source.atoms;
    fine.kind = source.kind;
    fine.axesInverse = glm::inverse(fineAxes);
    for (int axis = 0; axis < 3; ++axis) {
        fine.axes[axis] = glm::vec3(fineAxes[axis]);
    }
    const std::size_t voxelCount = static_cast<std::size_t>(dims[0]) *
                                   static_cast<std::size_t>(dims[1]) *
                                   static_cast<std::size_t>(dims[2]);
    fine.data.resize(voxelCount);
    const glm::dmat3 fineToSource = glm::inverse(sourceAxes) * fineAxes;
    resampleDensityTrilinear(source, fine.dims, fineToSource, fine.data);
    out = std::move(fine);
    return true;
}

// ---------------------------------------------------------------------------
// Marching Cubes 抽取（density == isolevel；位置量化合并为唯一顶点）
// ---------------------------------------------------------------------------

struct SharedSurface {
    std::vector<glm::vec3> positions;   // 唯一顶点（世界坐标 Å）
    std::vector<uint32_t>  triangles;   // 每三角形 3 个唯一顶点索引
};

bool extractIsoSurface(const entity::CubeVolume& density, float isolevel, SharedSurface& out) {
    out.positions.clear();
    out.triangles.clear();

    const int nx = density.dims[0];
    const int ny = density.dims[1];
    const int nz = density.dims[2];
    const float* values = density.data.data();
    const std::size_t strideX = static_cast<std::size_t>(ny) * nz;
    const std::size_t strideY = static_cast<std::size_t>(nz);
    // 8 个角在数据区中的下标偏移（顺序与 mc::kCorner 一致：(x,y,z) 局部 0/1）
    const std::size_t cornerOffset[8] = {0,
                                         strideX,
                                         strideX + strideY,
                                         strideY,
                                         1,
                                         strideX + 1,
                                         strideX + strideY + 1,
                                         strideY + 1};

    const glm::dvec3 origin = glm::dvec3(density.origin);
    const glm::dvec3 axis0 = glm::dvec3(density.axes[0]);
    const glm::dvec3 axis1 = glm::dvec3(density.axes[1]);
    const glm::dvec3 axis2 = glm::dvec3(density.axes[2]);

    std::unordered_map<uint64_t, uint32_t> uniqueIndex;
    const std::size_t voxelCount = static_cast<std::size_t>(nx) * ny * nz;
    uniqueIndex.reserve(static_cast<std::size_t>(
        std::cbrt(static_cast<double>(voxelCount) * static_cast<double>(voxelCount))));

    // 顶点合并：MC 顶点不共享，相邻立方体会重复生成同一条边交点 → 按 1e-4 Å 量化去重
    const auto vertexIndexFor = [&](const glm::vec3& position) -> uint32_t {
        const int64_t qx = static_cast<int64_t>(
            std::llround(static_cast<double>(position.x) * kMergeQuant));
        const int64_t qy = static_cast<int64_t>(
            std::llround(static_cast<double>(position.y) * kMergeQuant));
        const int64_t qz = static_cast<int64_t>(
            std::llround(static_cast<double>(position.z) * kMergeQuant));
        const uint64_t key = positionHash(qx, qy, qz);
        const auto found = uniqueIndex.find(key);
        if (found != uniqueIndex.end()) {
            return found->second;
        }
        const uint32_t index = static_cast<uint32_t>(out.positions.size());
        out.positions.push_back(position);
        uniqueIndex.emplace(key, index);
        return index;
    };

    uint32_t edgeIndex[12] = {};
    for (int ix = 0; ix + 1 < nx; ++ix) {
        for (int iy = 0; iy + 1 < ny; ++iy) {
            const std::size_t rowBase = static_cast<std::size_t>(ix) * strideX +
                                        static_cast<std::size_t>(iy) * strideY;
            for (int iz = 0; iz + 1 < nz; ++iz) {
                const float* cell = values + rowBase + static_cast<std::size_t>(iz);
                float cornerValue[8];
                for (int corner = 0; corner < 8; ++corner) {
                    cornerValue[corner] = cell[cornerOffset[corner]];
                }
                int cubeIndex = 0;
                for (int corner = 0; corner < 8; ++corner) {
                    if (cornerValue[corner] >= isolevel) {
                        cubeIndex |= (1 << corner);
                    }
                }
                if (mc::kEdgeTable[cubeIndex] == 0) {
                    continue;
                }

                // 8 角世界坐标（由索引确定性计算：相邻立方体对同一角点得到完全相同 float）
                glm::vec3 cornerPosition[8];
                for (int corner = 0; corner < 8; ++corner) {
                    const int cx = ix + (mc::kCorner[corner][0] > 0.5f ? 1 : 0);
                    const int cy = iy + (mc::kCorner[corner][1] > 0.5f ? 1 : 0);
                    const int cz = iz + (mc::kCorner[corner][2] > 0.5f ? 1 : 0);
                    cornerPosition[corner] = glm::vec3(
                        origin + axis0 * static_cast<double>(cx) +
                        axis1 * static_cast<double>(cy) + axis2 * static_cast<double>(cz));
                }

                for (int edge = 0; edge < 12; ++edge) {
                    if ((mc::kEdgeTable[cubeIndex] & (1 << edge)) == 0) {
                        continue;
                    }
                    const int a = mc::kEdgeVerts[edge][0];
                    const int b = mc::kEdgeVerts[edge][1];
                    const float va = cornerValue[a];
                    const float vb = cornerValue[b];
                    const float denominator = vb - va;
                    float t = std::abs(denominator) > 1e-20f ? (isolevel - va) / denominator : 0.5f;
                    if (!std::isfinite(t)) {
                        t = 0.5f;
                    }
                    t = std::min(std::max(t, 0.0f), 1.0f);
                    const glm::vec3 position =
                        cornerPosition[a] + (cornerPosition[b] - cornerPosition[a]) * t;
                    edgeIndex[edge] = vertexIndexFor(position);
                }

                for (int t = 0; t < 15 && mc::kTriTable[cubeIndex][t] != -1; t += 3) {
                    out.triangles.push_back(edgeIndex[mc::kTriTable[cubeIndex][t]]);
                    out.triangles.push_back(edgeIndex[mc::kTriTable[cubeIndex][t + 1]]);
                    out.triangles.push_back(edgeIndex[mc::kTriTable[cubeIndex][t + 2]]);
                }
            }
        }
    }

    // 过滤退化三角形（同一顶点重复：极少数边界/数值情况）
    std::vector<uint32_t> filtered;
    filtered.reserve(out.triangles.size());
    for (std::size_t i = 0; i + 2 < out.triangles.size(); i += 3) {
        const uint32_t a = out.triangles[i];
        const uint32_t b = out.triangles[i + 1];
        const uint32_t c = out.triangles[i + 2];
        if (a == b || b == c || c == a) {
            continue;
        }
        filtered.push_back(a);
        filtered.push_back(b);
        filtered.push_back(c);
    }
    out.triangles = std::move(filtered);
    return !out.positions.empty() && !out.triangles.empty();
}

// ---------------------------------------------------------------------------
// Taubin 平滑（唯一顶点邻接图；边界顶点固定防收缩）
// ---------------------------------------------------------------------------

struct VertexGraph {
    std::vector<uint32_t> offsets;    // CSR 偏移（size = vertexCount + 1）
    std::vector<uint32_t> neighbors;  // 去重后的邻接顶点
    std::vector<uint8_t>  pinned;     // 1 = 边界顶点（开放面边缘，平滑时保持不动）
};

void buildVertexGraph(const SharedSurface& surface, VertexGraph& graph) {
    const std::size_t vertexCount = surface.positions.size();
    graph.offsets.assign(vertexCount + 1, 0);
    graph.pinned.assign(vertexCount, 0);
    graph.neighbors.clear();
    if (vertexCount == 0) {
        return;
    }

    // 1) 统计每个顶点在三角形边中出现的次数（含重复边，用于去重/边界判定）
    const std::vector<uint32_t>& triangles = surface.triangles;
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const uint32_t a = triangles[i];
        const uint32_t b = triangles[i + 1];
        const uint32_t c = triangles[i + 2];
        ++graph.offsets[a + 1];
        ++graph.offsets[b + 1];
        ++graph.offsets[c + 1];
        ++graph.offsets[a + 1];
        ++graph.offsets[b + 1];
        ++graph.offsets[c + 1];
    }
    for (std::size_t i = 1; i < graph.offsets.size(); ++i) {
        graph.offsets[i] += graph.offsets[i - 1];
    }

    // 2) 填充（每条无向边给两个端点各记一次）
    std::vector<uint32_t> rawNeighbors(graph.offsets.back());
    std::vector<uint32_t> cursor = graph.offsets;
    const auto addEdge = [&](uint32_t from, uint32_t to) {
        if (from == to) {
            return;
        }
        rawNeighbors[cursor[from]++] = to;
        rawNeighbors[cursor[to]++] = from;
    };
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const uint32_t a = triangles[i];
        const uint32_t b = triangles[i + 1];
        const uint32_t c = triangles[i + 2];
        addEdge(a, b);
        addEdge(b, c);
        addEdge(c, a);
    }

    // 3) 邻接去重 + 边界判定：闭合流形中每条边被两个三角形共享（同一邻接出现 2 次）；
    //    只出现 1 次 = 开放边界（等值面被网格边界裁剪）→ 平滑时固定该顶点。
    std::vector<uint32_t> compacted;
    compacted.reserve(rawNeighbors.size());
    std::vector<uint32_t> newOffsets(vertexCount + 1, 0);
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const std::size_t begin = graph.offsets[vertex];
        const std::size_t end = graph.offsets[vertex + 1];
        std::sort(rawNeighbors.begin() + static_cast<std::ptrdiff_t>(begin),
                  rawNeighbors.begin() + static_cast<std::ptrdiff_t>(end));
        std::size_t k = begin;
        while (k < end) {
            std::size_t m = k + 1;
            while (m < end && rawNeighbors[m] == rawNeighbors[k]) {
                ++m;
            }
            if (m - k == 1) {
                graph.pinned[vertex] = 1;
            }
            compacted.push_back(rawNeighbors[k]);
            k = m;
        }
        newOffsets[vertex + 1] = static_cast<uint32_t>(compacted.size());
    }
    graph.offsets = std::move(newOffsets);
    graph.neighbors = std::move(compacted);
}

// Taubin 平滑：每轮先 λ=0.5 低通、再 μ=-0.53 反向补偿（防体积收缩）
void taubinSmooth(SharedSurface& surface, const VertexGraph& graph) {
    const std::size_t vertexCount = surface.positions.size();
    if (vertexCount == 0 || graph.offsets.size() != vertexCount + 1) {
        return;
    }
    std::vector<glm::vec3> buffer(vertexCount);
    const auto smoothPass = [&](const std::vector<glm::vec3>& source,
                                std::vector<glm::vec3>& target, float factor) {
        for (std::size_t index = 0; index < vertexCount; ++index) {
            if (graph.pinned[index] != 0) {
                target[index] = source[index];
                continue;
            }
            const uint32_t begin = graph.offsets[index];
            const uint32_t end = graph.offsets[index + 1];
            if (end <= begin) {
                target[index] = source[index];
                continue;
            }
            glm::vec3 sum(0.0f);
            for (uint32_t k = begin; k < end; ++k) {
                sum += source[graph.neighbors[k]];
            }
            const glm::vec3 average = sum / static_cast<float>(end - begin);
            target[index] = source[index] + (average - source[index]) * factor;
        }
    };
    for (int iteration = 0; iteration < kTaubinIterations; ++iteration) {
        smoothPass(surface.positions, buffer, kTaubinLambda);
        smoothPass(buffer, surface.positions, kTaubinMu);
    }
}

// ---------------------------------------------------------------------------
// 平滑后用细网格密度梯度重算法线（三线性采样 ±1 体素中心差分）
// ---------------------------------------------------------------------------

void computeSurfaceNormals(const entity::CubeVolume& density,
                           const glm::dmat3& worldToFractional, const glm::vec3& centerOfMass,
                           const std::vector<glm::vec3>& positions,
                           std::vector<glm::vec3>& normals) {
    normals.resize(positions.size());
    const glm::dvec3 axes[3] = {glm::dvec3(density.axes[0]), glm::dvec3(density.axes[1]),
                                glm::dvec3(density.axes[2])};
    const glm::dvec3 origin = glm::dvec3(density.origin);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const glm::vec3& position = positions[index];
        const glm::dvec3 fractional = worldToFractional * (glm::dvec3(position) - origin);
        glm::dvec3 gradient(0.0);
        bool hasGradient = false;
        for (int axis = 0; axis < 3; ++axis) {
            const double length2 = glm::dot(axes[axis], axes[axis]);
            if (!(length2 > 1e-20)) {
                continue;
            }
            glm::dvec3 forward = fractional;
            glm::dvec3 backward = fractional;
            forward[axis] += 1.0;
            backward[axis] -= 1.0;
            const float plus = sampleTrilinearClamped(density, forward);
            const float minus = sampleTrilinearClamped(density, backward);
            if (!std::isfinite(plus) || !std::isfinite(minus)) {
                continue;
            }
            // dρ/ds_axis = (plus - minus) / (2|axes|) → grad += axes * (plus-minus)/(2|axes|²)
            gradient += axes[axis] * (static_cast<double>(plus - minus) / (2.0 * length2));
            hasGradient = true;
        }
        glm::vec3 normal = radialDirection(position, centerOfMass);
        if (hasGradient) {
            const glm::vec3 outward = -glm::vec3(gradient);   // 密度梯度指向内部
            const float outwardLength2 = glm::dot(outward, outward);
            if (outwardLength2 > kTinyLength2) {
                normal = outward / std::sqrt(outwardLength2);
            }
        }
        normals[index] = normal;
    }
}

// 三角形绕序与法线一致 + 全局朝外（法线整体指向质心时统一翻转）
void orientSurface(SharedSurface& surface, std::vector<glm::vec3>& normals,
                   const glm::vec3& centerOfMass) {
    std::vector<uint32_t>& triangles = surface.triangles;
    const std::vector<glm::vec3>& positions = surface.positions;
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const uint32_t a = triangles[i];
        const uint32_t b = triangles[i + 1];
        const uint32_t c = triangles[i + 2];
        const glm::vec3 geometric =
            glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
        if (glm::dot(geometric, normals[a] + normals[b] + normals[c]) < 0.0f) {
            std::swap(triangles[i + 1], triangles[i + 2]);
        }
    }
    double orientation = 0.0;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        orientation +=
            static_cast<double>(glm::dot(normals[index], positions[index] - centerOfMass));
    }
    if (orientation < 0.0) {
        for (glm::vec3& normal : normals) {
            normal = -normal;
        }
        for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
            std::swap(triangles[i + 1], triangles[i + 2]);
        }
    }
}

// ---------------------------------------------------------------------------
// 色标范围 / 逐顶点颜色
// ---------------------------------------------------------------------------

float percentileOf(std::vector<float> values, float percent) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    const float clamped = std::min(std::max(percent, 0.0f), 100.0f);
    const float position = clamped * 0.01f * static_cast<float>(values.size() - 1);
    const std::size_t i0 = static_cast<std::size_t>(std::max(0.0f, std::floor(position)));
    const std::size_t i1 = std::min(i0 + 1, values.size() - 1);
    const float frac = position - static_cast<float>(i0);
    return values[i0] + (values[i1] - values[i0]) * frac;
}

// 计算实际色标范围：autoRange 时发散型取 ±99.5 分位 |v|，顺序型取 [1, 99] 分位；
// 否则沿用参数 vmin/vmax（非法/退化时兜底扩展，保证 vmin < vmax）。
void computeColorRange(const EspSurfaceParams& params, const std::vector<float>& values,
                       float& outVminKcal, float& outVmaxKcal) {
    float fallbackMin = params.vminKcal;
    float fallbackMax = params.vmaxKcal;
    if (!std::isfinite(fallbackMin)) {
        fallbackMin = -45.0f;
    }
    if (!std::isfinite(fallbackMax)) {
        fallbackMax = 45.0f;
    }
    if (!(fallbackMax > fallbackMin)) {
        const float middle = 0.5f * (fallbackMin + fallbackMax);
        fallbackMin = middle - 1.0f;
        fallbackMax = middle + 1.0f;
    }

    if (!params.autoRange || values.empty()) {
        outVminKcal = fallbackMin;
        outVmaxKcal = fallbackMax;
        return;
    }

    if (isDivergentColormap(params.colormap)) {
        std::vector<float> magnitudes;
        magnitudes.reserve(values.size());
        for (float value : values) {
            if (std::isfinite(value)) {
                magnitudes.push_back(std::abs(value));
            }
        }
        float limit = percentileOf(std::move(magnitudes), 99.5f);
        if (!std::isfinite(limit) || limit < 1e-3f) {
            limit = 1.0f;
        }
        outVminKcal = -limit;
        outVmaxKcal = limit;
        return;
    }

    std::vector<float> finiteValues;
    finiteValues.reserve(values.size());
    for (float value : values) {
        if (std::isfinite(value)) {
            finiteValues.push_back(value);
        }
    }
    if (finiteValues.empty()) {
        outVminKcal = fallbackMin;
        outVmaxKcal = fallbackMax;
        return;
    }
    float lo = percentileOf(finiteValues, 1.0f);
    float hi = percentileOf(std::move(finiteValues), 99.0f);
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) {
        const float middle = (std::isfinite(lo) && std::isfinite(hi)) ? 0.5f * (lo + hi) : 0.0f;
        lo = middle - 1.0f;
        hi = middle + 1.0f;
    }
    outVminKcal = lo;
    outVmaxKcal = hi;
}

// ---------------------------------------------------------------------------
// 极值点检测（唯一顶点 1e-4 Å 量化去重 + 1.2 Å 空间哈希 + 非极大抑制）
// ---------------------------------------------------------------------------

// 空间哈希网格单元键（单元边长 = 极值邻域半径；21 位/轴足够表示 ±1000 Å）
uint64_t cellHash(int32_t cx, int32_t cy, int32_t cz) noexcept {
    constexpr int64_t kCellOffset = 1 << 20;
    const uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(cx) + kCellOffset) & 0x1FFFFFULL;
    const uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(cy) + kCellOffset) & 0x1FFFFFULL;
    const uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(cz) + kCellOffset) & 0x1FFFFFULL;
    return (x << 42) | (y << 21) | z;
}

int32_t extremumCellCoord(float value) noexcept {
    return static_cast<int32_t>(std::floor(value / kExtremumRadius));
}

struct RawVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float     valueKcal = 0.0f;   // 该顶点 ESP（kcal/mol）
};

struct UniqueSample {
    glm::vec3 position{0.0f};
    float     valueKcal = 0.0f;
};

struct ExtremumCandidate {
    glm::vec3 position{0.0f};
    float     valueKcal = 0.0f;
};

// 紧凑空间哈希：开放寻址索引 + 桶内链（等价 unordered_map<uint64_t, vector>）。
// 极值检测要做约 26 万次 27 邻域查询，Debug 构建下 unordered_map 是主要热点；
// 该结构每次查询只做 1~2 次探测与两次数组读，语义保持不变。
class CellIndex {
public:
    void build(const std::vector<std::array<int32_t, 3>>& cells) {
        std::size_t capacity = 64;
        while (capacity < cells.size() * 2 + 8) {
            capacity <<= 1;
        }
        mask_ = static_cast<uint32_t>(capacity - 1);
        keys_.assign(capacity, 0);
        heads_.assign(capacity, 0);
        next_.assign(cells.size(), 0);
        for (uint32_t sample = 0; sample < cells.size(); ++sample) {
            const uint64_t stored =
                cellHash(cells[sample][0], cells[sample][1], cells[sample][2]) + 1;
            uint32_t slot = static_cast<uint32_t>(stored ^ (stored >> 32)) & mask_;
            while (keys_[slot] != 0 && keys_[slot] != stored) {
                slot = (slot + 1) & mask_;
            }
            keys_[slot] = stored;
            next_[sample] = heads_[slot];
            heads_[slot] = sample + 1;   // 1-based；0 表示空
        }
    }

    // 返回桶内首个样本（1-based；0 = 该单元没有样本），用 next() 遍历链
    uint32_t first(uint64_t cellKey) const {
        const uint64_t stored = cellKey + 1;
        uint32_t slot = static_cast<uint32_t>(stored ^ (stored >> 32)) & mask_;
        while (keys_[slot] != 0) {
            if (keys_[slot] == stored) {
                return heads_[slot];
            }
            slot = (slot + 1) & mask_;
        }
        return 0;
    }

    uint32_t next(uint32_t node) const { return next_[node - 1]; }

private:
    std::vector<uint64_t> keys_;
    std::vector<uint32_t> heads_;
    std::vector<uint32_t> next_;
    uint32_t              mask_ = 63;
};

std::vector<EspExtremum> detectExtrema(const std::vector<RawVertex>& vertices) {
    std::vector<EspExtremum> extrema;
    if (vertices.empty()) {
        return extrema;
    }

    // 1) 按 1e-4 Å 量化去重（平滑后的唯一顶点在此基本不会再合并，保留兜底）
    std::vector<UniqueSample> samples;
    samples.reserve(vertices.size() / 2 + 1);
    std::unordered_map<uint64_t, uint32_t> sampleIndex;
    sampleIndex.reserve(vertices.size() / 2 + 1);
    for (const RawVertex& vertex : vertices) {
        const int64_t qx = static_cast<int64_t>(
            std::llround(static_cast<double>(vertex.position.x) * kExtremumQuant));
        const int64_t qy = static_cast<int64_t>(
            std::llround(static_cast<double>(vertex.position.y) * kExtremumQuant));
        const int64_t qz = static_cast<int64_t>(
            std::llround(static_cast<double>(vertex.position.z) * kExtremumQuant));
        const uint64_t key = positionHash(qx, qy, qz);
        const auto found = sampleIndex.find(key);
        if (found != sampleIndex.end()) {
            UniqueSample& existing = samples[found->second];
            if (std::abs(vertex.valueKcal) > std::abs(existing.valueKcal)) {
                existing.valueKcal = vertex.valueKcal;
            }
            continue;
        }
        sampleIndex.emplace(key, static_cast<uint32_t>(samples.size()));
        UniqueSample sample;
        sample.position = vertex.position;
        sample.valueKcal = vertex.valueKcal;
        samples.push_back(sample);
    }

    // 2) 空间哈希：单元边长 = 1.2 Å，邻居只需查 27 个相邻单元
    std::vector<std::array<int32_t, 3>> sampleCells(samples.size());
    for (uint32_t i = 0; i < samples.size(); ++i) {
        sampleCells[i] = {extremumCellCoord(samples[i].position.x),
                          extremumCellCoord(samples[i].position.y),
                          extremumCellCoord(samples[i].position.z)};
    }
    CellIndex cellIndex;
    cellIndex.build(sampleCells);

    // 3) 局部极大/极小判定（邻域内无更大/更小值；平坦平台不算极值）
    std::vector<ExtremumCandidate> maxima;
    std::vector<ExtremumCandidate> minima;
    for (uint32_t i = 0; i < samples.size(); ++i) {
        const glm::vec3& position = samples[i].position;
        const float value = samples[i].valueKcal;
        bool isMaximum = true;
        bool isMinimum = true;
        const std::array<int32_t, 3>& cell = sampleCells[i];
        for (int dx = -1; dx <= 1 && (isMaximum || isMinimum); ++dx) {
            for (int dy = -1; dy <= 1 && (isMaximum || isMinimum); ++dy) {
                for (int dz = -1; dz <= 1 && (isMaximum || isMinimum); ++dz) {
                    const uint32_t bucket =
                        cellIndex.first(cellHash(cell[0] + dx, cell[1] + dy, cell[2] + dz));
                    for (uint32_t node = bucket; node != 0; node = cellIndex.next(node)) {
                        const uint32_t j = node - 1;
                        if (j == i) {
                            continue;
                        }
                        const glm::vec3 delta = samples[j].position - position;
                        if (glm::dot(delta, delta) > kExtremumRadius2) {
                            continue;
                        }
                        if (samples[j].valueKcal > value) {
                            isMaximum = false;
                        }
                        if (samples[j].valueKcal < value) {
                            isMinimum = false;
                        }
                    }
                }
            }
        }
        if (isMaximum && isMinimum) {
            continue;   // 邻域等值平台，不算极值
        }
        if (isMaximum) {
            maxima.push_back(ExtremumCandidate{position, value});
        }
        if (isMinimum) {
            minima.push_back(ExtremumCandidate{position, value});
        }
    }

    // 4) 非极大抑制：按 |value| 从大到小贪心选取，1.2 Å 内只保留最强的一个
    const auto selectTop = [](std::vector<ExtremumCandidate> candidates, bool isMaximum,
                              const std::vector<glm::vec3>& blocked) {
        std::vector<EspExtremum> picked;
        std::sort(candidates.begin(), candidates.end(),
                  [](const ExtremumCandidate& a, const ExtremumCandidate& b) {
                      return std::abs(a.valueKcal) > std::abs(b.valueKcal);
                  });
        for (const ExtremumCandidate& candidate : candidates) {
            if (picked.size() >= kMaxExtremaPerType) {
                break;
            }
            // 极小值排除 0（ESP 为 0 的点不具化学意义）
            if (!isMaximum && std::abs(candidate.valueKcal) <= 1e-9f) {
                continue;
            }
            bool clash = false;
            for (const EspExtremum& existing : picked) {
                const glm::vec3 delta = existing.position - candidate.position;
                if (glm::dot(delta, delta) <= kExtremumRadius2) {
                    clash = true;
                    break;
                }
            }
            if (!clash) {
                for (const glm::vec3& blockedPosition : blocked) {
                    const glm::vec3 delta = blockedPosition - candidate.position;
                    if (glm::dot(delta, delta) <= kExtremumRadius2) {
                        clash = true;
                        break;
                    }
                }
            }
            if (clash) {
                continue;
            }
            EspExtremum extremum;
            extremum.position = candidate.position;
            extremum.valueKcal = candidate.valueKcal;
            extremum.isMaximum = isMaximum;
            picked.push_back(extremum);
        }
        return picked;
    };

    std::vector<EspExtremum> pickedMaxima = selectTop(std::move(maxima), true, {});
    std::vector<glm::vec3> blockedPositions;
    blockedPositions.reserve(pickedMaxima.size());
    for (const EspExtremum& extremum : pickedMaxima) {
        blockedPositions.push_back(extremum.position);
    }
    std::vector<EspExtremum> pickedMinima = selectTop(std::move(minima), false, blockedPositions);

    extrema.reserve(pickedMaxima.size() + pickedMinima.size());
    extrema.insert(extrema.end(), pickedMaxima.begin(), pickedMaxima.end());
    extrema.insert(extrema.end(), pickedMinima.begin(), pickedMinima.end());
    return extrema;
}

} // namespace

// ---------------------------------------------------------------------------
// 色标公开接口
// ---------------------------------------------------------------------------

const char* espColormapName(EspColormap colormap) noexcept {
    switch (colormap) {
        case EspColormap::Rwb:
            return "rwb";
        case EspColormap::Bwr:
            return "bwr";
        case EspColormap::Coolwarm:
            return "coolwarm";
        case EspColormap::Turbo:
            return "turbo";
        case EspColormap::Viridis:
            return "viridis";
        case EspColormap::Jet:
            return "jet";
        case EspColormap::Reference:
            return "reference";
        case EspColormap::Count:
        default:
            return "rwb";
    }
}

const char* espColormapLabelZh(EspColormap colormap) noexcept {
    switch (colormap) {
        case EspColormap::Rwb:
            return "红-白-蓝（化学惯例）";
        case EspColormap::Bwr:
            return "蓝-白-红";
        case EspColormap::Coolwarm:
            return "冷暖";
        case EspColormap::Turbo:
            return "Turbo";
        case EspColormap::Viridis:
            return "Viridis";
        case EspColormap::Jet:
            return "Jet";
        case EspColormap::Reference:
            return "参考配色（黄→绿→青→蓝→紫）";
        case EspColormap::Count:
        default:
            return "红-白-蓝（化学惯例）";
    }
}

EspColormap espColormapFromName(const std::string& name) noexcept {
    const auto equalsIgnoreCase = [&name](const char* literal) noexcept {
        std::size_t i = 0;
        for (; literal[i] != '\0'; ++i) {
            if (i >= name.size()) {
                return false;
            }
            char c = name[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != literal[i]) {
                return false;
            }
        }
        return i == name.size();
    };
    if (equalsIgnoreCase("rwb")) {
        return EspColormap::Rwb;
    }
    if (equalsIgnoreCase("bwr")) {
        return EspColormap::Bwr;
    }
    if (equalsIgnoreCase("coolwarm")) {
        return EspColormap::Coolwarm;
    }
    if (equalsIgnoreCase("turbo")) {
        return EspColormap::Turbo;
    }
    if (equalsIgnoreCase("viridis")) {
        return EspColormap::Viridis;
    }
    if (equalsIgnoreCase("jet")) {
        return EspColormap::Jet;
    }
    if (equalsIgnoreCase("reference") || equalsIgnoreCase("spectral") ||
        equalsIgnoreCase("rainbow")) {
        return EspColormap::Reference;
    }
    return EspColormap::Rwb;
}

glm::vec3 espColormapColor(EspColormap colormap, float t) {
    const EspColormap sanitized = sanitizeColormap(static_cast<unsigned>(colormap));
    const StopTable table = stopsFor(sanitized);
    float x = t;
    if (!std::isfinite(x)) {
        x = 0.0f;
    }
    x = std::min(std::max(x, 0.0f), 1.0f);

    std::size_t segment = 0;
    while (segment + 2 < table.count && x > table.data[segment + 1].t) {
        ++segment;
    }
    const ColorStop& from = table.data[segment];
    const ColorStop& to = table.data[segment + 1];
    const float span = to.t - from.t;
    float frac = span > 1e-6f ? (x - from.t) / span : 0.0f;
    frac = std::min(std::max(frac, 0.0f), 1.0f);
    return glm::vec3(from.color.r + (to.color.r - from.color.r) * frac,
                     from.color.g + (to.color.g - from.color.g) * frac,
                     from.color.b + (to.color.b - from.color.b) * frac);
}

// ---------------------------------------------------------------------------
// 表面构建（带缓存；细网格 + Taubin 平滑）
// ---------------------------------------------------------------------------

bool buildEspSurface(const EspSurfaceParams& params, EspSurfaceMesh& out, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        out = EspSurfaceMesh{};
        return false;
    };

    // 1) 参数校验
    if (params.densityCubePath.empty()) {
        return fail("ESP 表面：未指定电子密度 cube 文件路径");
    }
    if (params.potentialCubePath.empty()) {
        return fail("ESP 表面：未指定静电势 cube 文件路径");
    }
    if (!std::isfinite(params.isolevel)) {
        return fail("ESP 表面：密度等值面阈值非法（NaN/Inf）");
    }

    // 2) 缓存查询（键 = 两路径 + 全部参数 + 两文件 size/mtime；缓存的是最终网格）
    BuildKey key;
    key.densityPath = params.densityCubePath;
    key.potentialPath = params.potentialCubePath;
    key.isolevel = params.isolevel;
    key.vminKcal = params.vminKcal;
    key.vmaxKcal = params.vmaxKcal;
    key.autoRange = params.autoRange;
    key.invert = params.invert;
    key.colormap = sanitizeColormap(static_cast<unsigned>(params.colormap));
    key.densityFile = fingerprintOf(params.densityCubePath);
    key.potentialFile = fingerprintOf(params.potentialCubePath);
    if (cacheLookup(key, out)) {
        return true;
    }
    // 串行化 cache miss 的构建：并发线程可能同时到达这里，先到者负责构建，
    // 后到者在拿到锁后重新查一次缓存即可命中（也避免 cube 解析器被并发调用）。
    std::lock_guard<std::mutex> buildLock(g_buildMutex);
    if (cacheLookup(key, out)) {
        return true;
    }

    // 3) 解析两个 cube（失败 → false + error，不抛异常）
    entity::CubeVolume density;
    std::string parseError;
    if (!entity::parseCubeFile(params.densityCubePath, density, &parseError)) {
        return fail("解析密度 cube 失败（" + params.densityCubePath + "）：" + parseError);
    }
    if (!density.valid()) {
        return fail("密度 cube 数据无效（dims/data 不匹配）：" + params.densityCubePath);
    }
    entity::CubeVolume potential;
    if (!entity::parseCubeFile(params.potentialCubePath, potential, &parseError)) {
        return fail("解析静电势 cube 失败（" + params.potentialCubePath + "）：" + parseError);
    }
    if (!potential.valid()) {
        return fail("静电势 cube 数据无效（dims/data 不匹配）：" + params.potentialCubePath);
    }

    // 4) 等值面阈值必须在密度数据范围内，否则直接失败（避免空 mesh 无解释）
    float densityMin = 0.0f;
    float densityMax = 0.0f;
    if (!computeDataRange(density, densityMin, densityMax)) {
        return fail("密度 cube 不含有效数值：" + params.densityCubePath);
    }
    if (params.isolevel < densityMin || params.isolevel > densityMax) {
        return fail("密度等值面阈值 " + std::to_string(params.isolevel) +
                    " 超出密度范围 [" + std::to_string(densityMin) + ", " +
                    std::to_string(densityMax) + "]");
    }

    // 5) 细网格重采样（只针对密度；ESP 仍用原 cube 采样，避免二次插值误差累积）
    entity::CubeVolume workingDensity;
    if (!buildWorkingDensity(density, workingDensity)) {
        workingDensity = std::move(density);   // 原网格已足够细 / 无法重采样：直接使用
    }

    // 6) Marching Cubes（唯一顶点）→ Taubin 平滑 → 细网格梯度重算法线
    SharedSurface surface;
    if (!extractIsoSurface(workingDensity, params.isolevel, surface)) {
        return fail("未生成等值面（isolevel=" + std::to_string(params.isolevel) +
                    " 可能不适合该密度 cube）");
    }
    VertexGraph graph;
    buildVertexGraph(surface, graph);
    taubinSmooth(surface, graph);

    glm::dmat3 workingWorldToFractional(1.0);
    const bool hasWorkingXform =
        computeWorldToFractional(workingDensity, workingWorldToFractional);
    const glm::vec3 centerOfMass = computeCenterOfMass(workingDensity);
    std::vector<glm::vec3> normals;
    if (hasWorkingXform) {
        computeSurfaceNormals(workingDensity, workingWorldToFractional, centerOfMass,
                              surface.positions, normals);
    } else {
        normals.resize(surface.positions.size());
        for (std::size_t index = 0; index < surface.positions.size(); ++index) {
            normals[index] = radialDirection(surface.positions[index], centerOfMass);
        }
    }
    orientSurface(surface, normals, centerOfMass);

    // 7) ESP 逐唯一顶点采样 → 色标范围 → 逐顶点颜色
    glm::dmat3 potentialWorldToFractional(1.0);
    const bool hasPotentialXform = computeWorldToFractional(potential, potentialWorldToFractional);
    const glm::dmat3* potentialXform = hasPotentialXform ? &potentialWorldToFractional : nullptr;
    std::vector<float> values(surface.positions.size());
    for (std::size_t index = 0; index < surface.positions.size(); ++index) {
        values[index] = samplePotentialKcal(potential, potentialXform, surface.positions[index]);
    }

    float vminKcal = 0.0f;
    float vmaxKcal = 0.0f;
    computeColorRange(params, values, vminKcal, vmaxKcal);
    const float span = vmaxKcal - vminKcal;
    const float inverseSpan = span > 1e-12f ? 1.0f / span : 0.0f;
    const EspColormap colormap = sanitizeColormap(static_cast<unsigned>(params.colormap));
    std::vector<glm::vec3> colors(surface.positions.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        float t = (values[index] - vminKcal) * inverseSpan;
        if (!std::isfinite(t)) {
            t = 0.5f;
        }
        t = std::min(std::max(t, 0.0f), 1.0f);
        if (params.invert) {
            t = 1.0f - t;
        }
        glm::vec3 color = espColormapColor(colormap, t);
        if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z)) {
            color = glm::vec3(1.0f);   // 防御：非法颜色回退白色
        }
        colors[index] = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    // 8) 极值点检测（平滑后的唯一顶点；NMS 逻辑不变）
    std::vector<RawVertex> extremumInput(surface.positions.size());
    for (std::size_t index = 0; index < surface.positions.size(); ++index) {
        extremumInput[index].position = surface.positions[index];
        extremumInput[index].normal = normals[index];
        extremumInput[index].valueKcal = values[index];
    }

    // 9) 展开为 TriMesh 布局（顶点不共享：每三角形 3 个顶点 + 3 个索引）
    const std::size_t triangleCount = surface.triangles.size() / 3;
    EspSurfaceMesh mesh;
    mesh.positions.resize(triangleCount * 3);
    mesh.normals.resize(triangleCount * 3);
    mesh.colors.resize(triangleCount * 3);
    mesh.indices.resize(triangleCount * 3);
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
        for (int corner = 0; corner < 3; ++corner) {
            const std::size_t outIndex = triangle * 3 + static_cast<std::size_t>(corner);
            const uint32_t vertex = surface.triangles[outIndex];
            mesh.positions[outIndex] = surface.positions[vertex];
            mesh.normals[outIndex] = normals[vertex];
            mesh.colors[outIndex] = colors[vertex];
            mesh.indices[outIndex] = static_cast<uint32_t>(outIndex);
        }
    }
    mesh.vminKcal = vminKcal;
    mesh.vmaxKcal = vmaxKcal;
    mesh.extrema = detectExtrema(extremumInput);

    // 10) 写缓存并返回（缓存命中路径同样返回完整 mesh）
    cacheStore(key, mesh);
    out = std::move(mesh);
    return true;
}

void clearEspSurfaceCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.clear();
}

// ---------------------------------------------------------------------------
// 样式入口：RepresentationType::EspSurface
// ---------------------------------------------------------------------------

void drawEspSurface(const RepContext& ctx, SceneCommandList& out) {
    if (ctx.rep == nullptr) {
        return;
    }
    const Representation& representation = *ctx.rep;
    if (representation.espDensityCubePath.empty() ||
        representation.espPotentialCubePath.empty()) {
        return;   // 参数为空：不绘制
    }

    EspSurfaceParams params;
    params.densityCubePath = representation.espDensityCubePath;
    params.potentialCubePath = representation.espPotentialCubePath;
    params.isolevel = representation.espIsovalue;
    params.vminKcal = representation.espVminKcal;
    params.vmaxKcal = representation.espVmaxKcal;
    params.autoRange = representation.espAutoRange;
    params.invert = representation.espInvert;
    params.colormap = sanitizeColormap(static_cast<unsigned>(representation.espColormap));

    EspSurfaceMesh mesh;
    std::string error;
    if (!buildEspSurface(params, mesh, &error)) {
        return;
    }
    if (mesh.positions.empty() || mesh.indices.empty()) {
        return;
    }

    // 调试：MOLECULENS_ESP_DUMP=<path.obj> 时导出网格（排查渲染伪影/几何质量用）
    if (const char* dumpPath = std::getenv("MOLECULENS_ESP_DUMP")) {
        if (std::FILE* file = std::fopen(dumpPath, "wb")) {
            for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
                const glm::vec3& p = mesh.positions[i];
                const glm::vec3 c = i < mesh.colors.size() ? mesh.colors[i] : glm::vec3(1.0f);
                std::fprintf(file, "v %g %g %g %g %g %g\n", p.x, p.y, p.z, c.r, c.g, c.b);
            }
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const unsigned a = mesh.indices[i] + 1;
                const unsigned b = mesh.indices[i + 1] + 1;
                const unsigned c = mesh.indices[i + 2] + 1;
                std::fprintf(file, "f %u %u %u\n", a, b, c);
            }
            std::fclose(file);
        }
    }

    std::vector<TriMeshVertex> vertices(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        vertices[i].pos = glm::vec4(mesh.positions[i], 0.0f);
        const glm::vec3 normal =
            i < mesh.normals.size() ? mesh.normals[i] : glm::vec3(0.0f, 0.0f, 1.0f);
        vertices[i].normal = glm::vec4(normal, 0.0f);
        const glm::vec3 color = i < mesh.colors.size() ? mesh.colors[i] : glm::vec3(1.0f);
        // ESP 色标同样按 sRGB 定义，而颜色附件是 *_SRGB（写入时硬件再编码一次），
        // 所以这里转线性，避免 ESP 表面比其它表示明显偏亮。
        auto toLin = [](float v) -> float {
            return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        };
        vertices[i].color =
            glm::vec4(toLin(color.r), toLin(color.g), toLin(color.b), 1.0f);   // alpha=1（半透明由材质/管线控制）
    }
    out.appendTriMesh(vertices, mesh.indices);

    // 成功后写入视口叠加信息（缓存命中路径也执行到这里）
    EspOverlay overlay;
    overlay.active = true;
    overlay.colormap = params.colormap;
    overlay.vminKcal = mesh.vminKcal;
    overlay.vmaxKcal = mesh.vmaxKcal;
    overlay.invert = params.invert;
    overlay.unit = "kcal/mol";
    overlay.extrema = mesh.extrema;
    espOverlaySet(overlay);
}

// ---------------------------------------------------------------------------
// 视口叠加信息（色标条 + 极值标签）
// ---------------------------------------------------------------------------

void espOverlayReset() {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    g_overlay = EspOverlay{};
}

void espOverlaySet(const EspOverlay& overlay) {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    g_overlay = overlay;
}

EspOverlay espOverlaySnapshot() {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    return g_overlay;
}

} // namespace moleculens::engine
