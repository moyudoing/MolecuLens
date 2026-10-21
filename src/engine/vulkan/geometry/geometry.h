// =============================================================================
// MolecuLens · engine · vulkan/geometry/geometry.h —— 后端共享几何与投影数学
// 规格来源：主方案 §5.7.1（icosphere 细分）/ §5.7.3（圆柱）/ §6.3（Vulkan 深度范围）
//
// 职责：本模块是**与 VkDevice 无关**的纯数学/几何生成，供实时与离屏两个后端共用。
//       在本次重构前，下列函数在 vulkan_renderer.cpp 与 offscreen_renderer.cpp
//       中各有一份逐行相同的实现（约 130 行 × 2），任何修正都要改两处——
//       vkProjection 的深度重映射 bug 历史上正因两份实现而各修一遍。
//       收敛到此处后为唯一实现。
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::engine {

// 单位 icosphere（半径 1）→ 顶点 + 三角形索引。
// detail：细分档位。1 = 80 面（最省），2 = 320，3 = 1280（默认，导出清晰），
//         4 = 5120（放大 5–10 倍仍圆）。
void generateIcosphere(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices,
                       int detail = 3);

// 单位圆柱（半径 1，轴向 y ∈ [-1, 1]，**无端盖**）→ 顶点 + 三角形索引。
// segments：截面段数。8 = 省，24 = 默认，48 = 高倍放大导出用。
// 键圆柱几何：由 cylinder.vert 以端点 A/B 与半径把单位圆柱变换到位。
void generateCylinder(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices,
                      int segments = 24);

// Vulkan 投影修正：Y 轴翻转 + 深度范围 [-1,1] → [0,1]。
//
// ⚠ 必须作用于**纯投影矩阵**，再左乘视图矩阵；直接作用于 P*V 联合矩阵只会翻
//   m[1][1]，把视图的 up 分量也翻错 → clip.y 错误 → 分子不可见。
//
// ⚠ 深度重映射的读写方向极易写反：GLM 列主序（m[col][row]）中 z 行的 w 系数在
//   m[3][2]，w 行的 z 系数在 m[2][3]。写反会让 clip.w 变成 -0.5z、深度映射全错，
//   几何被远平面整体裁剪——而**清屏仍然正常**，表现为"视口空白"极难定位
//   （见 moleculens-fixes 第四轮）。故收敛为唯一实现并以此注释固化约束。
glm::mat4 vkProjection(const glm::mat4& proj);

} // namespace moleculens::engine
