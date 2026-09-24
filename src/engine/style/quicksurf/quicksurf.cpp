// =============================================================================
// MolecuLens · engine · style/quicksurf/quicksurf.cpp —— QuickSurf 高斯表面实现
// 规格来源：参考 other/QuickSurf（高斯密度和；radscale/gridspacing/isovalue）
// 算法：对原子集合计算高斯密度场（含密度梯度），Marching Cubes 抽取 isovalue
//       等值面 → TriMesh。密度/梯度计算按后端分派：CUDA（启用时）→ AVX2（检测到）
//       → 标量多线程（默认，按原子块并行 + 归约）。
// =============================================================================

#include "engine/style/quicksurf/quicksurf.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

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
    const unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
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

// 后端分派（运行时自动选择：CUDA → AVX2 → 标量；或 quickSurfSetBackend 强制）
void computeDensityDispatch(const float* xyzr, int natoms, const float* colors,
                            float radscale, float gridspacing, float gausslim,
                            const float origin[3], const int dims[3],
                            float* density, float* grad, float* colormap) {
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

    for (int z = 0; z + 1 < nz; ++z) {
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
                    const int v0 = mc::kTriTable[cubeIndex][t];
                    const int v1 = mc::kTriTable[cubeIndex][t + 1];
                    const int v2 = mc::kTriTable[cubeIndex][t + 2];
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v0]);
                    nrm.push_back(edgeNrm[v0]);
                    if (hasColor) {
                        colors.push_back(edgeCol[v0]);
                    }
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v1]);
                    nrm.push_back(edgeNrm[v1]);
                    if (hasColor) {
                        colors.push_back(edgeCol[v1]);
                    }
                    indices.push_back(static_cast<uint32_t>(verts.size()));
                    verts.push_back(edgePos[v2]);
                    nrm.push_back(edgeNrm[v2]);
                    if (hasColor) {
                        colors.push_back(edgeCol[v2]);
                    }
                }
            }
        }
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
    computeDensityDispatch(xyzr.data(), static_cast<int>(centers.size()), nullptr,
                           radscale, sp, kGaussLim, o, d, density.data(), grad.data(),
                           nullptr);
    return extractSurface(density, grad, colormap, false, isovalue, origin, dims, sp,
                          out);
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
    TriMeshHeader hdr;
    hdr.vertexCount = static_cast<uint32_t>(mesh.positions.size());
    hdr.indexCount = static_cast<uint32_t>(mesh.indices.size());
    std::vector<uint8_t> payload;
    payload.reserve(sizeof(TriMeshHeader) +
                    mesh.positions.size() * sizeof(TriMeshVertex) +
                    mesh.indices.size() * sizeof(uint32_t));
    const uint8_t* hdrp = reinterpret_cast<const uint8_t*>(&hdr);
    payload.insert(payload.end(), hdrp, hdrp + sizeof(TriMeshHeader));
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        TriMeshVertex tv;
        tv.pos = glm::vec4(mesh.positions[i], 0.0f);
        tv.normal = glm::vec4(mesh.normals[i], 0.0f);
        // 密度加权原子色（QuickSurf 体积色）；无则浅蓝
        const glm::vec3 c = i < mesh.colors.size() ? mesh.colors[i] : glm::vec3(0.55f, 0.75f, 0.95f);
        tv.color = glm::vec4(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
        const uint8_t* vp = reinterpret_cast<const uint8_t*>(&tv);
        payload.insert(payload.end(), vp, vp + sizeof(TriMeshVertex));
    }
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(mesh.indices.data());
    payload.insert(payload.end(), ip, ip + mesh.indices.size() * sizeof(uint32_t));
    while (payload.size() % 16 != 0) {
        payload.push_back(0);
    }
    out.append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));
}

// 体系级 QuickSurf（G5）：聚合全部可见实例原子（含溶剂分子）为一个整体高斯密度表面
// （而不是每分子一个独立小表面）。用于溶剂化大体系（如 md.gro）。
void drawSystemQuickSurf(const RepContext& ctx, SceneCommandList& out) {
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
    std::vector<glm::vec3> colors;
    const auto& insts = ctx.scene->instances();
    const size_t total = ctx.frame && ctx.frame->positions ? ctx.frame->positions->size() : 0;
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
            // 元素 CPK 色（体系级无 RepContext.instance → 用元素色近似，与
            // drawQuickSurf 的密度加权原子色同源）
            const auto& el = elementInfo(st.elements[i]);
            colors.emplace_back(el.color[0], el.color[1], el.color[2]);
        }
        globalBase += n;
    }
    if (centers.empty()) {
        return;
    }

    // 密度/梯度/颜色计算（后端自动选择：CUDA → AVX2 → 标量 MT）
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

    // TriMesh 输出（法线 + 密度加权原子色，与 drawQuickSurf 一致）
    TriMeshHeader hdr;
    hdr.vertexCount = static_cast<uint32_t>(mesh.positions.size());
    hdr.indexCount = static_cast<uint32_t>(mesh.indices.size());
    std::vector<uint8_t> payload;
    payload.reserve(sizeof(TriMeshHeader) +
                    mesh.positions.size() * sizeof(TriMeshVertex) +
                    mesh.indices.size() * sizeof(uint32_t));
    const uint8_t* hdrp = reinterpret_cast<const uint8_t*>(&hdr);
    payload.insert(payload.end(), hdrp, hdrp + sizeof(TriMeshHeader));
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        TriMeshVertex tv;
        tv.pos = glm::vec4(mesh.positions[i], 0.0f);
        tv.normal = glm::vec4(mesh.normals[i], 0.0f);
        const glm::vec3 c = i < mesh.colors.size() ? mesh.colors[i]
                                                   : glm::vec3(0.55f, 0.75f, 0.95f);
        tv.color = glm::vec4(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
        const uint8_t* vp = reinterpret_cast<const uint8_t*>(&tv);
        payload.insert(payload.end(), vp, vp + sizeof(TriMeshVertex));
    }
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(mesh.indices.data());
    payload.insert(payload.end(), ip, ip + mesh.indices.size() * sizeof(uint32_t));
    while (payload.size() % 16 != 0) {
        payload.push_back(0);
    }
    out.append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));
}

} // namespace moleculens::engine
