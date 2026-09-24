// =============================================================================
// MolecuLens · engine · style/surface/surface.cpp —— vdW 表面（Marching Cubes）
// 规格来源：主方案 §5.7.6（vdW 表面 / SES；输出 TriMesh）
// 实现：标量场 f(p) = min_i(|p-c_i| - r_i)（到原子 vdW 球面的有符号距离），
//       Marching Cubes 抽取 f==0 等值面；顶点法线 = 指向最近原子中心。
// =============================================================================

#include "engine/style/surface/surface.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

namespace {

// —— Marching Cubes 标准查找表（Paul Bourke 公开实现，公共领域）——
const int kEdgeTable[256] = {
    0x0, 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
    0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
    0x190, 0x99, 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
    0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
    0x230, 0x339, 0x33, 0x13a, 0x636, 0x73f, 0x435, 0x53c,
    0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
    0x3a0, 0x2a9, 0x1a3, 0xaa, 0x7a6, 0x6af, 0x5a5, 0x4ac,
    0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
    0x460, 0x569, 0x663, 0x76a, 0x66, 0x16f, 0x265, 0x36c,
    0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
    0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff, 0x3f5, 0x2fc,
    0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
    0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55, 0x15c,
    0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
    0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc,
    0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
    0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
    0xcc, 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
    0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
    0x15c, 0x55, 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
    0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
    0x2fc, 0x3f5, 0xff, 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
    0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
    0x36c, 0x265, 0x16f, 0x66, 0x76a, 0x663, 0x569, 0x460,
    0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
    0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa, 0x1a3, 0x2a9, 0x3a0,
    0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
    0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33, 0x339, 0x230,
    0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
    0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99, 0x190,
    0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
    0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0};

const int kTriTable[256][16] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 8, 3, 9, 8, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 2, 10, 0, 2, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 8, 3, 2, 10, 8, 10, 9, 8, -1, -1, -1, -1, -1, -1, -1},
    {3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 11, 2, 8, 11, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 11, 2, 1, 9, 11, 9, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {3, 10, 1, 11, 10, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 10, 1, 0, 8, 10, 8, 11, 10, -1, -1, -1, -1, -1, -1, -1},
    {3, 9, 0, 3, 11, 9, 11, 10, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 3, 0, 7, 3, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 1, 9, 4, 7, 1, 7, 3, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 4, 7, 3, 0, 4, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 2, 10, 9, 0, 2, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
    {2, 10, 9, 2, 9, 7, 2, 7, 3, 7, 9, 4, -1, -1, -1, -1},
    {8, 4, 7, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 4, 7, 11, 2, 4, 2, 0, 4, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 1, 8, 4, 7, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
    {4, 7, 11, 9, 4, 11, 9, 11, 2, 9, 2, 1, -1, -1, -1, -1},
    {3, 10, 1, 3, 11, 10, 7, 8, 4, -1, -1, -1, -1, -1, -1, -1},
    {1, 11, 10, 1, 4, 11, 1, 0, 4, 7, 11, 4, -1, -1, -1, -1},
    {4, 7, 8, 9, 0, 11, 9, 11, 10, 11, 0, 3, -1, -1, -1, -1},
    {4, 7, 11, 4, 11, 9, 9, 11, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 5, 4, 1, 5, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 5, 4, 8, 3, 5, 3, 1, 5, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 1, 2, 10, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
    {5, 2, 10, 5, 4, 2, 4, 0, 2, -1, -1, -1, -1, -1, -1, -1},
    {2, 10, 5, 3, 2, 5, 3, 5, 4, 3, 4, 8, -1, -1, -1, -1},
    {9, 5, 4, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 11, 2, 0, 8, 11, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
    {0, 5, 4, 0, 1, 5, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
    {2, 1, 5, 2, 5, 8, 2, 8, 11, 4, 8, 5, -1, -1, -1, -1},
    {10, 3, 11, 10, 1, 3, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 5, 0, 8, 1, 8, 10, 1, 8, 11, 10, -1, -1, -1, -1},
    {5, 4, 0, 5, 0, 11, 5, 11, 10, 11, 0, 3, -1, -1, -1, -1},
    {5, 4, 8, 5, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {9, 7, 8, 5, 7, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 3, 0, 9, 5, 3, 5, 7, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 7, 8, 0, 1, 7, 1, 5, 7, -1, -1, -1, -1, -1, -1, -1},
    {1, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 7, 8, 9, 5, 7, 10, 1, 2, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 9, 5, 0, 5, 3, 0, 5, 7, 3, -1, -1, -1, -1},
    {8, 0, 2, 8, 2, 5, 8, 5, 7, 10, 5, 2, -1, -1, -1, -1},
    {2, 10, 5, 2, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1},
    {7, 9, 5, 7, 8, 9, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 7, 9, 7, 2, 9, 2, 0, 2, 7, 11, -1, -1, -1, -1},
    {2, 3, 11, 0, 1, 8, 1, 7, 8, 1, 5, 7, -1, -1, -1, -1},
    {11, 2, 1, 11, 1, 7, 7, 1, 5, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 8, 8, 5, 7, 10, 1, 3, 10, 3, 11, -1, -1, -1, -1},
    {5, 7, 0, 5, 0, 9, 7, 11, 0, 1, 0, 10, 11, 10, 0, -1},
    {11, 10, 0, 11, 0, 3, 10, 5, 0, 8, 0, 7, 5, 7, 0, -1},
    {11, 10, 5, 7, 11, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 1, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 8, 3, 1, 9, 8, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 5, 2, 6, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 5, 1, 2, 6, 3, 0, 8, -1, -1, -1, -1, -1, -1, -1},
    {9, 6, 5, 9, 0, 6, 0, 2, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 9, 8, 5, 8, 2, 5, 2, 6, 3, 2, 8, -1, -1, -1, -1},
    {2, 3, 11, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 0, 8, 11, 2, 0, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 1, 9, 2, 9, 11, 2, 9, 8, 11, -1, -1, -1, -1},
    {6, 3, 11, 6, 5, 3, 5, 1, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 11, 0, 11, 5, 0, 5, 1, 5, 11, 6, -1, -1, -1, -1},
    {3, 11, 6, 0, 3, 6, 0, 6, 5, 0, 5, 9, -1, -1, -1, -1},
    {6, 5, 9, 6, 9, 11, 11, 9, 8, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 3, 0, 4, 7, 3, 6, 5, 10, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 5, 10, 6, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
    {10, 6, 5, 1, 9, 7, 1, 7, 3, 7, 9, 4, -1, -1, -1, -1},
    {6, 1, 2, 6, 5, 1, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 5, 5, 2, 6, 3, 0, 4, 3, 4, 7, -1, -1, -1, -1},
    {8, 4, 7, 9, 0, 5, 0, 6, 5, 0, 2, 6, -1, -1, -1, -1},
    {7, 3, 9, 7, 9, 4, 3, 2, 9, 5, 9, 6, 2, 6, 9, -1},
    {3, 11, 2, 7, 8, 4, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 4, 7, 2, 4, 2, 0, 2, 7, 11, -1, -1, -1, -1},
    {0, 1, 9, 4, 7, 8, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1},
    {9, 2, 1, 9, 11, 2, 9, 4, 11, 7, 11, 4, 5, 10, 6, -1},
    {8, 4, 7, 3, 11, 5, 3, 5, 1, 5, 11, 6, -1, -1, -1, -1},
    {5, 1, 11, 5, 11, 6, 1, 0, 11, 7, 11, 4, 0, 4, 11, -1},
    {0, 5, 9, 0, 6, 5, 0, 3, 6, 11, 6, 3, 8, 4, 7, -1},
    {6, 5, 9, 6, 9, 11, 4, 7, 9, 7, 11, 9, -1, -1, -1, -1},
    {10, 4, 9, 6, 4, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 10, 6, 4, 9, 10, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1},
    {10, 0, 1, 10, 6, 0, 6, 4, 0, -1, -1, -1, -1, -1, -1, -1},
    {8, 3, 1, 8, 1, 6, 8, 6, 4, 6, 1, 10, -1, -1, -1, -1},
    {1, 4, 9, 1, 2, 4, 2, 6, 4, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 1, 2, 9, 2, 4, 9, 2, 6, 4, -1, -1, -1, -1},
    {0, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 3, 2, 8, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1},
    {10, 4, 9, 10, 6, 4, 11, 2, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 2, 2, 8, 11, 4, 9, 10, 4, 10, 6, -1, -1, -1, -1},
    {3, 11, 2, 0, 1, 6, 0, 6, 4, 6, 1, 10, -1, -1, -1, -1},
    {6, 4, 1, 6, 1, 10, 4, 8, 1, 2, 1, 11, 8, 11, 1, -1},
    {9, 6, 4, 9, 3, 6, 9, 1, 3, 11, 6, 3, -1, -1, -1, -1},
    {8, 11, 1, 8, 1, 0, 11, 6, 1, 9, 1, 4, 6, 4, 1, -1},
    {3, 11, 6, 3, 6, 0, 0, 6, 4, -1, -1, -1, -1, -1, -1, -1},
    {6, 4, 8, 11, 6, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 10, 6, 7, 8, 10, 8, 9, 10, -1, -1, -1, -1, -1, -1, -1},
    {0, 7, 3, 0, 10, 7, 0, 9, 10, 6, 7, 10, -1, -1, -1, -1},
    {10, 6, 7, 1, 10, 7, 1, 7, 8, 1, 8, 0, -1, -1, -1, -1},
    {10, 6, 7, 10, 7, 1, 1, 7, 3, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 6, 1, 6, 8, 1, 8, 9, 8, 6, 7, -1, -1, -1, -1},
    {2, 6, 9, 2, 9, 1, 6, 7, 9, 0, 9, 3, 7, 3, 9, -1},
    {7, 8, 0, 7, 0, 6, 6, 0, 2, -1, -1, -1, -1, -1, -1, -1},
    {7, 3, 2, 6, 7, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 11, 10, 6, 8, 10, 8, 9, 8, 6, 7, -1, -1, -1, -1},
    {2, 0, 7, 2, 7, 11, 0, 9, 7, 6, 7, 10, 9, 10, 7, -1},
    {1, 8, 0, 1, 7, 8, 1, 10, 7, 6, 7, 10, 2, 3, 11, -1},
    {11, 2, 1, 11, 1, 7, 10, 6, 1, 6, 7, 1, -1, -1, -1, -1},
    {8, 9, 6, 8, 6, 7, 9, 1, 6, 11, 6, 3, 1, 3, 6, -1},
    {0, 9, 1, 11, 6, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 8, 0, 7, 0, 6, 3, 11, 0, 11, 6, 0, -1, -1, -1, -1},
    {7, 11, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 1, 9, 8, 3, 1, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 3, 0, 8, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
    {2, 9, 0, 2, 10, 9, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
    {6, 11, 7, 2, 10, 3, 10, 8, 3, 10, 9, 8, -1, -1, -1, -1},
    {7, 2, 3, 6, 2, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 0, 8, 7, 6, 0, 6, 2, 0, -1, -1, -1, -1, -1, -1, -1},
    {2, 7, 6, 2, 3, 7, 0, 1, 9, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 2, 1, 8, 6, 1, 9, 8, 8, 7, 6, -1, -1, -1, -1},
    {10, 7, 6, 10, 1, 7, 1, 3, 7, -1, -1, -1, -1, -1, -1, -1},
    {10, 7, 6, 1, 7, 10, 1, 8, 7, 1, 0, 8, -1, -1, -1, -1},
    {0, 3, 7, 0, 7, 10, 0, 10, 9, 6, 10, 7, -1, -1, -1, -1},
    {7, 6, 10, 7, 10, 8, 8, 10, 9, -1, -1, -1, -1, -1, -1, -1},
    {6, 8, 4, 11, 8, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 6, 11, 3, 0, 6, 0, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {8, 6, 11, 8, 4, 6, 9, 0, 1, -1, -1, -1, -1, -1, -1, -1},
    {9, 4, 6, 9, 6, 3, 9, 3, 1, 11, 3, 6, -1, -1, -1, -1},
    {6, 8, 4, 6, 11, 8, 2, 10, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 3, 0, 11, 0, 6, 11, 0, 4, 6, -1, -1, -1, -1},
    {4, 11, 8, 4, 6, 11, 0, 2, 9, 2, 10, 9, -1, -1, -1, -1},
    {10, 9, 3, 10, 3, 2, 9, 4, 3, 11, 3, 6, 4, 6, 3, -1},
    {8, 2, 3, 8, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1},
    {0, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1, -1, -1, -1},
    {1, 9, 4, 1, 4, 2, 2, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {8, 1, 3, 8, 6, 1, 8, 4, 6, 6, 10, 1, -1, -1, -1, -1},
    {10, 1, 0, 10, 0, 6, 6, 0, 4, -1, -1, -1, -1, -1, -1, -1},
    {4, 6, 3, 4, 3, 8, 6, 10, 3, 0, 3, 9, 10, 9, 3, -1},
    {10, 9, 4, 6, 10, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 5, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 4, 9, 5, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 1, 5, 4, 0, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {11, 7, 6, 8, 3, 4, 3, 5, 4, 3, 1, 5, -1, -1, -1, -1},
    {9, 5, 4, 10, 1, 2, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {6, 11, 7, 1, 2, 10, 0, 8, 3, 4, 9, 5, -1, -1, -1, -1},
    {7, 6, 11, 5, 4, 10, 4, 2, 10, 4, 0, 2, -1, -1, -1, -1},
    {3, 4, 8, 3, 5, 4, 3, 2, 5, 10, 5, 2, 11, 7, 6, -1},
    {7, 2, 3, 7, 6, 2, 5, 4, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, 0, 8, 6, 0, 6, 2, 6, 8, 7, -1, -1, -1, -1},
    {3, 6, 2, 3, 7, 6, 1, 5, 0, 5, 4, 0, -1, -1, -1, -1},
    {6, 2, 8, 6, 8, 7, 2, 1, 8, 4, 8, 5, 1, 5, 8, -1},
    {9, 5, 4, 10, 1, 6, 1, 7, 6, 1, 3, 7, -1, -1, -1, -1},
    {1, 6, 10, 1, 7, 6, 1, 0, 7, 8, 7, 0, 9, 5, 4, -1},
    {4, 0, 10, 4, 10, 5, 0, 3, 10, 6, 10, 7, 3, 7, 10, -1},
    {7, 6, 10, 7, 10, 8, 5, 4, 10, 4, 8, 10, -1, -1, -1, -1},
    {6, 9, 5, 6, 11, 9, 11, 8, 9, -1, -1, -1, -1, -1, -1, -1},
    {3, 6, 11, 0, 6, 3, 0, 5, 6, 0, 9, 5, -1, -1, -1, -1},
    {0, 11, 8, 0, 5, 11, 0, 1, 5, 5, 6, 11, -1, -1, -1, -1},
    {6, 11, 3, 6, 3, 5, 5, 3, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 9, 5, 11, 9, 11, 8, 11, 5, 6, -1, -1, -1, -1},
    {0, 11, 3, 0, 6, 11, 0, 9, 6, 5, 6, 9, 1, 2, 10, -1},
    {11, 8, 5, 11, 5, 6, 8, 0, 5, 10, 5, 2, 0, 2, 5, -1},
    {6, 11, 3, 6, 3, 5, 2, 10, 3, 10, 5, 3, -1, -1, -1, -1},
    {5, 8, 9, 5, 2, 8, 5, 6, 2, 3, 8, 2, -1, -1, -1, -1},
    {9, 5, 6, 9, 6, 0, 0, 6, 2, -1, -1, -1, -1, -1, -1, -1},
    {1, 5, 8, 1, 8, 0, 5, 6, 8, 3, 8, 2, 6, 2, 8, -1},
    {1, 5, 6, 2, 1, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 3, 6, 1, 6, 10, 3, 8, 6, 5, 6, 9, 8, 9, 6, -1},
    {10, 1, 0, 10, 0, 6, 9, 5, 0, 5, 6, 0, -1, -1, -1, -1},
    {0, 3, 8, 5, 6, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {10, 5, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 5, 10, 7, 5, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 5, 10, 11, 7, 5, 8, 3, 0, -1, -1, -1, -1, -1, -1, -1},
    {5, 11, 7, 5, 10, 11, 1, 9, 0, -1, -1, -1, -1, -1, -1, -1},
    {10, 7, 5, 10, 11, 7, 9, 8, 1, 8, 3, 1, -1, -1, -1, -1},
    {11, 1, 2, 11, 7, 1, 7, 5, 1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 1, 2, 7, 1, 7, 5, 7, 2, 11, -1, -1, -1, -1},
    {9, 7, 5, 9, 2, 7, 9, 0, 2, 2, 11, 7, -1, -1, -1, -1},
    {7, 5, 2, 7, 2, 11, 5, 9, 2, 3, 2, 8, 9, 8, 2, -1},
    {2, 5, 10, 2, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1},
    {8, 2, 0, 8, 5, 2, 8, 7, 5, 10, 2, 5, -1, -1, -1, -1},
    {9, 0, 1, 5, 10, 3, 5, 3, 7, 3, 10, 2, -1, -1, -1, -1},
    {9, 8, 2, 9, 2, 1, 8, 7, 2, 10, 2, 5, 7, 5, 2, -1},
    {1, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 7, 0, 7, 1, 1, 7, 5, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 3, 9, 3, 5, 5, 3, 7, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 7, 5, 9, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {5, 8, 4, 5, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 4, 5, 11, 0, 5, 10, 11, 11, 3, 0, -1, -1, -1, -1},
    {0, 1, 9, 8, 4, 10, 8, 10, 11, 10, 4, 5, -1, -1, -1, -1},
    {10, 11, 4, 10, 4, 5, 11, 3, 4, 9, 4, 1, 3, 1, 4, -1},
    {2, 5, 1, 2, 8, 5, 2, 11, 8, 4, 5, 8, -1, -1, -1, -1},
    {0, 4, 11, 0, 11, 3, 4, 5, 11, 2, 11, 1, 5, 1, 11, -1},
    {0, 2, 5, 0, 5, 9, 2, 11, 5, 4, 5, 8, 11, 8, 5, -1},
    {9, 4, 5, 2, 11, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 5, 10, 3, 5, 2, 3, 4, 5, 3, 8, 4, -1, -1, -1, -1},
    {5, 10, 2, 5, 2, 4, 4, 2, 0, -1, -1, -1, -1, -1, -1, -1},
    {3, 10, 2, 3, 5, 10, 3, 8, 5, 4, 5, 8, 0, 1, 9, -1},
    {5, 10, 2, 5, 2, 4, 1, 9, 2, 9, 4, 2, -1, -1, -1, -1},
    {8, 4, 5, 8, 5, 3, 3, 5, 1, -1, -1, -1, -1, -1, -1, -1},
    {0, 4, 5, 1, 0, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 4, 5, 8, 5, 3, 9, 0, 5, 0, 3, 5, -1, -1, -1, -1},
    {9, 4, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 11, 7, 4, 9, 11, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 4, 9, 7, 9, 11, 7, 9, 10, 11, -1, -1, -1, -1},
    {1, 10, 11, 1, 11, 4, 1, 4, 0, 7, 4, 11, -1, -1, -1, -1},
    {3, 1, 4, 3, 4, 8, 1, 10, 4, 7, 4, 11, 10, 11, 4, -1},
    {4, 11, 7, 9, 11, 4, 9, 2, 11, 9, 1, 2, -1, -1, -1, -1},
    {9, 7, 4, 9, 11, 7, 9, 1, 11, 2, 11, 1, 0, 8, 3, -1},
    {11, 7, 4, 11, 4, 2, 2, 4, 0, -1, -1, -1, -1, -1, -1, -1},
    {11, 7, 4, 11, 4, 2, 8, 3, 4, 3, 2, 4, -1, -1, -1, -1},
    {2, 9, 10, 2, 7, 9, 2, 3, 7, 7, 4, 9, -1, -1, -1, -1},
    {9, 10, 7, 9, 7, 4, 10, 2, 7, 8, 7, 0, 2, 0, 7, -1},
    {3, 7, 10, 3, 10, 2, 7, 4, 10, 1, 10, 0, 4, 0, 10, -1},
    {1, 10, 2, 8, 7, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 1, 4, 1, 7, 7, 1, 3, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 1, 4, 1, 7, 0, 8, 1, 8, 7, 1, -1, -1, -1, -1},
    {4, 0, 3, 7, 4, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 8, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 9, 3, 9, 11, 11, 9, 10, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 10, 0, 10, 8, 8, 10, 11, -1, -1, -1, -1, -1, -1, -1},
    {3, 1, 10, 11, 3, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 11, 1, 11, 9, 9, 11, 8, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 9, 3, 9, 11, 1, 2, 9, 2, 11, 9, -1, -1, -1, -1},
    {0, 2, 11, 8, 0, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 2, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 8, 2, 8, 10, 10, 8, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 10, 2, 0, 9, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 8, 2, 8, 10, 0, 1, 8, 1, 10, 8, -1, -1, -1, -1},
    {1, 10, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 3, 8, 9, 1, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 9, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 3, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};

// 单立方体 12 条边的端点（MC 标准约定：0-11）
const int kEdgeVerts[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},   // 底面
    {4, 5}, {5, 6}, {6, 7}, {7, 4},   // 顶面
    {0, 4}, {1, 5}, {2, 6}, {3, 7}};  // 竖直

// 立方体 8 个角（局部坐标 0..1）
const glm::vec3 kCorner[8] = {
    {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
    {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};

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

    // 收集选中原子中心与 vdW 半径
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t g = ctx.globalAtomBase + static_cast<uint32_t>(i);
        if (g >= ctx.mask->size() || !(*ctx.mask)[g]) {
            continue;
        }
        centers.push_back(pos[g]);
        radii.push_back(elementInfo(st.elements[i]).vdwRadius * ctx.rep->radiusScale);
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

    // 网格（spacing 自适应，限制最大维度防爆网格）
    constexpr float kTargetSpacing = 0.8f;
    constexpr int kMaxDim = 96;
    const glm::vec3 size = hi - lo;
    float sp = kTargetSpacing;
    const int maxDim = std::max({static_cast<int>(size.x / sp) + 1,
                                 static_cast<int>(size.y / sp) + 1,
                                 static_cast<int>(size.z / sp) + 1});
    if (maxDim > kMaxDim) {
        sp *= static_cast<float>(maxDim) / static_cast<float>(kMaxDim);
    }
    const glm::ivec3 dims(std::max(2, static_cast<int>(size.x / sp) + 1),
                          std::max(2, static_cast<int>(size.y / sp) + 1),
                          std::max(2, static_cast<int>(size.z / sp) + 1));
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

    // 12 条边在立方体坐标中的端角索引（用 kEdgeVerts）
    for (int z = 0; z + 1 < nz; ++z) {
        for (int y = 0; y + 1 < ny; ++y) {
            for (int x = 0; x + 1 < nx; ++x) {
                // 8 个角的场值 + 立方体坐标
                glm::vec3 cornerPos[8];
                float cornerVal[8];
                for (int c = 0; c < 8; ++c) {
                    const int cx = x + (kCorner[c].x > 0.5f ? 1 : 0);
                    const int cy = y + (kCorner[c].y > 0.5f ? 1 : 0);
                    const int cz = z + (kCorner[c].z > 0.5f ? 1 : 0);
                    cornerVal[c] = field[idx(cx, cy, cz)];
                    cornerPos[c] = gridPos(cx, cy, cz);
                }
                int cubeIndex = 0;
                for (int c = 0; c < 8; ++c) {
                    if (cornerVal[c] < 0.0f) {
                        cubeIndex |= (1 << c);
                    }
                }
                if (kEdgeTable[cubeIndex] == 0) {
                    continue;
                }
                // 12 条边上的交点
                glm::vec3 edgePos[12];
                glm::vec3 edgeNrm[12];
                for (int e = 0; e < 12; ++e) {
                    if (!(kEdgeTable[cubeIndex] & (1 << e))) {
                        continue;
                    }
                    const int a = kEdgeVerts[e][0], b = kEdgeVerts[e][1];
                    edgePos[e] = interpolate(
                        x + (kCorner[a].x > 0.5f ? 1 : 0),
                        y + (kCorner[a].y > 0.5f ? 1 : 0),
                        z + (kCorner[a].z > 0.5f ? 1 : 0),
                        x + (kCorner[b].x > 0.5f ? 1 : 0),
                        y + (kCorner[b].y > 0.5f ? 1 : 0),
                        z + (kCorner[b].z > 0.5f ? 1 : 0),
                        cornerPos[a], cornerPos[b], &edgeNrm[e]);
                }
                // 三角形
                for (int t = 0; kTriTable[cubeIndex][t] != -1; t += 3) {
                    const int v0 = kTriTable[cubeIndex][t];
                    const int v1 = kTriTable[cubeIndex][t + 1];
                    const int v2 = kTriTable[cubeIndex][t + 2];
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

    // TriMesh 输出（法线 + 统一表面色）
    TriMeshHeader hdr;
    hdr.vertexCount = static_cast<uint32_t>(verts.size());
    hdr.indexCount = static_cast<uint32_t>(indices.size());
    std::vector<uint8_t> payload;
    payload.reserve(sizeof(TriMeshHeader) + verts.size() * sizeof(TriMeshVertex) +
                    indices.size() * sizeof(uint32_t));
    const uint8_t* hdrp = reinterpret_cast<const uint8_t*>(&hdr);
    payload.insert(payload.end(), hdrp, hdrp + sizeof(TriMeshHeader));
    const glm::vec4 surfColor(0.55f, 0.75f, 0.95f, 1.0f);   // 浅蓝表面
    for (size_t i = 0; i < verts.size(); ++i) {
        TriMeshVertex tv;
        tv.pos = glm::vec4(verts[i], 0.0f);
        tv.normal = glm::vec4(vertNrm[i], 0.0f);
        tv.color = surfColor;
        const uint8_t* vp = reinterpret_cast<const uint8_t*>(&tv);
        payload.insert(payload.end(), vp, vp + sizeof(TriMeshVertex));
    }
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(indices.data());
    payload.insert(payload.end(), ip, ip + indices.size() * sizeof(uint32_t));
    while (payload.size() % 16 != 0) {
        payload.push_back(0);
    }
    out.append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));
}

// —— 体系级 vdW 表面（G5）：聚合当前体系全部可见原子（含溶剂分子）为一个整体表面。——
// 用于大体系（如溶剂化 md.gro）：不再每分子生成一个独立小表面，而是整体系一个
// 封闭表面（溶剂也在其中），视觉连续、拓扑完整。
void drawSystemSurface(const RepContext& ctx, SceneCommandList& out) {
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }
    std::vector<glm::vec3> centers;
    std::vector<float> radii;
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

    // 网格（spacing 自适应，限制最大维度防爆网格）
    constexpr float kTargetSpacing = 0.8f;
    constexpr int kMaxDim = 96;
    const glm::vec3 size = hi - lo;
    float sp = kTargetSpacing;
    const int maxDim = std::max({static_cast<int>(size.x / sp) + 1,
                                 static_cast<int>(size.y / sp) + 1,
                                 static_cast<int>(size.z / sp) + 1});
    if (maxDim > kMaxDim) {
        sp *= static_cast<float>(maxDim) / static_cast<float>(kMaxDim);
    }
    const glm::ivec3 dims(std::max(2, static_cast<int>(size.x / sp) + 1),
                          std::max(2, static_cast<int>(size.y / sp) + 1),
                          std::max(2, static_cast<int>(size.z / sp) + 1));

    std::vector<float> field;
    std::vector<int> nearest;
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
                    const int cx = x + (kCorner[c].x > 0.5f ? 1 : 0);
                    const int cy = y + (kCorner[c].y > 0.5f ? 1 : 0);
                    const int cz = z + (kCorner[c].z > 0.5f ? 1 : 0);
                    cornerVal[c] = field[idx(cx, cy, cz)];
                    cornerPos[c] = gridPos(cx, cy, cz);
                }
                int cubeIndex = 0;
                for (int c = 0; c < 8; ++c) {
                    if (cornerVal[c] < 0.0f) {
                        cubeIndex |= (1 << c);
                    }
                }
                if (kEdgeTable[cubeIndex] == 0) {
                    continue;
                }
                glm::vec3 edgePos[12];
                glm::vec3 edgeNrm[12];
                for (int e = 0; e < 12; ++e) {
                    if (!(kEdgeTable[cubeIndex] & (1 << e))) {
                        continue;
                    }
                    const int a = kEdgeVerts[e][0], b = kEdgeVerts[e][1];
                    edgePos[e] = interpolate(x + (kCorner[a].x > 0.5f ? 1 : 0),
                                             y + (kCorner[a].y > 0.5f ? 1 : 0),
                                             z + (kCorner[a].z > 0.5f ? 1 : 0),
                                             x + (kCorner[b].x > 0.5f ? 1 : 0),
                                             y + (kCorner[b].y > 0.5f ? 1 : 0),
                                             z + (kCorner[b].z > 0.5f ? 1 : 0),
                                             cornerPos[a], cornerPos[b], &edgeNrm[e]);
                }
                for (int t = 0; kTriTable[cubeIndex][t] != -1; t += 3) {
                    const int v0 = kTriTable[cubeIndex][t];
                    const int v1 = kTriTable[cubeIndex][t + 1];
                    const int v2 = kTriTable[cubeIndex][t + 2];
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

    // TriMesh 输出（法线 + 统一表面色，与 drawSurface 一致）
    TriMeshHeader hdr;
    hdr.vertexCount = static_cast<uint32_t>(verts.size());
    hdr.indexCount = static_cast<uint32_t>(indices.size());
    std::vector<uint8_t> payload;
    payload.reserve(sizeof(TriMeshHeader) + verts.size() * sizeof(TriMeshVertex) +
                    indices.size() * sizeof(uint32_t));
    const uint8_t* hdrp = reinterpret_cast<const uint8_t*>(&hdr);
    payload.insert(payload.end(), hdrp, hdrp + sizeof(TriMeshHeader));
    const glm::vec4 surfColor(0.55f, 0.75f, 0.95f, 1.0f);   // 浅蓝表面
    for (size_t i = 0; i < verts.size(); ++i) {
        TriMeshVertex tv;
        tv.pos = glm::vec4(verts[i], 0.0f);
        tv.normal = glm::vec4(vertNrm[i], 0.0f);
        tv.color = surfColor;
        const uint8_t* vp = reinterpret_cast<const uint8_t*>(&tv);
        payload.insert(payload.end(), vp, vp + sizeof(TriMeshVertex));
    }
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(indices.data());
    payload.insert(payload.end(), ip, ip + indices.size() * sizeof(uint32_t));
    while (payload.size() % 16 != 0) {
        payload.push_back(0);
    }
    out.append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));
}

} // namespace moleculens::engine
