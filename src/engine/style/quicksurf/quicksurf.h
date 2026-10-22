// =============================================================================
// MolecuLens · engine · style/quicksurf/quicksurf.h —— QuickSurf 高斯表面
// 规格来源：参考 other/QuickSurf（高斯密度和等值面抽取；radscale/gridspacing/isovalue）
// 实现：对原子集合计算高斯密度场（density = Σ exp(-|p-c|²/(2(radscale·r)²))），
//       用 Marching Cubes 抽取密度 == isovalue 的等值面 → TriMesh。
//       密度与梯度在多线程/向量化/CUDA 后端中计算（见 quickSurfMesh）。
// =============================================================================

#pragma once

#include <vector>

#include <glm/glm.hpp>

#include "engine/cmd/cmd.h"

namespace moleculens::engine {

struct RepContext;

// QuickSurf 网格结果（顶点/法线/颜色/索引；法线 = 密度梯度方向，颜色 = 密度加权原子色）
struct QuickSurfMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec3> colors;   // 无原子色时为空
    std::vector<uint32_t> indices;
};

// 计算高斯密度等值面网格。
// centers/radii = 原子中心（Å）与 vdW 半径（Å）；
// radscale = 原子半径缩放；gridspacing = 网格间距（Å，越小越精细）；isovalue = 等值面阈值。
// 密度/梯度计算自动选择后端：CUDA（启用时）→ AVX2（支持时）→ 标量多线程（默认）。
bool quickSurfMesh(const std::vector<glm::vec3>& centers,
                   const std::vector<float>& radii,
                   float radscale, float gridspacing, float isovalue,
                   QuickSurfMesh& out);

// representer 分派（QuickSurf 渲染模式）
void drawQuickSurf(const RepContext& ctx, SceneCommandList& out);
// 体系级 QuickSurf（G5）：聚合全部可见实例原子（含溶剂）为一个整体高斯密度表面
void drawSystemQuickSurf(const RepContext& ctx, SceneCommandList& out);

// —— 异步生成开关（GUI 主线程专用；默认关闭 = 同步生成）——
// 开启后：整体系 QuickSurf 在网格缓存未命中时**提交后台任务并立即返回**（本次不发射
// 该表面），由 Renderer 在任务完成后自动重建 —— 于是生成期间旧命令流继续显示
// （旧画面不消失），主线程不再阻塞约 197 ms。
// 关闭时保持原行为（同步生成后再发射），CLI / 导出线程 / 单元测试都走这条路径。
// 注意：开关是进程级全局量，由 Renderer 在每次重建前按自身异步模式设置。
void quickSurfSetAsyncEnabled(bool enabled);
bool quickSurfAsyncEnabled();

// —— 运动中冻结表面几何（轨迹播放/拖时间轴）——
// 开启后，整体系 QuickSurf 若已有缓存网格则**直接复用**（不再按坐标指纹重算、
// 也不再提交后台任务）。轨迹每帧都改坐标，按原逻辑会每帧生成一次表面（实测
// md.gro 单次 ~730 ms）→ 播放被拖到 1–3 FPS。冻结期间球棍/线框照常逐帧更新，
// 表面保持上一帧；暂停后由 Renderer 关闭冻结并重建一次。
void quickSurfSetFreezeGeometry(bool frozen);
bool quickSurfFreezeGeometry();

// —— 计算后端（QS.2：CUDA → AVX2 → 标量多线程；运行时自动选择或强制指定）——

// 后端类型（Auto = 运行时自动选择；其它强制指定，便于测试/调优）
enum class QuickSurfBackend : int {
    Auto = 0,
    Scalar = 1,
    Avx2 = 2,
    Cuda = 3,
};

// 设置后端（0=Auto）；quickSurfBackend 返回当前生效的后端
void quickSurfSetBackend(int backend);
int quickSurfBackend();

// 各后端密度核（xyzr = 每原子 4 float：cx,cy,cz,radius；处理 [a0,a1) 原子）。
// 向 density/colormap 累加密度与密度加权原子色；grad 累加密度梯度（外向法线）。
void computeDensityScalarMT(const float* xyzr, int natoms, const float* colors, int a0, int a1,
                            float radscale, float gridspacing, float gausslim,
                            const float origin[3], const int dims[3],
                            float* density, float* grad, float* colormap);
bool avx2KernelAvailable();
void computeDensityAvx2(const float* xyzr, int natoms, const float* colors, int a0, int a1,
                        float radscale, float gridspacing, float gausslim,
                        const float origin[3], const int dims[3],
                        float* density, float* grad, float* colormap);
bool cudaKernelAvailable();
void computeDensityCuda(const float* xyzr, int natoms, const float* colors,
                        float radscale, float gridspacing, float gausslim,
                        const float origin[3], const int dims[3],
                        float* density, float* grad, float* colormap);

} // namespace moleculens::engine
