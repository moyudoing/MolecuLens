// =============================================================================
// MolecuLens · engine · export/stl/stl_exporter.cpp —— STL 导出实现
// 规格来源：主方案 §6.2（二进制 STL：80B 头 + u32 三角形数 + 每三角形
//           法线/3 顶点/属性）
//
// 几何来源：export/tessellate 的共享展开规则（球/圆柱由单位网格实例化展开，
//           TriMesh 直出）。此前本文件只处理 TriMesh，导致球棍/空间填充这类
//           以球实例为主的表示会**静默导出空文件**。
//
// 一致性：计数阶段与写出阶段共用同一遍历，保证头部声明的三角形数恒等于实际
//         写出的数量（此前两者用不同循环条件，在 indexCount 非 3 的倍数时不一致，
//         产出的文件会被多数工具判为损坏）。
// =============================================================================

#include "engine/export/stl/stl_exporter.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <glm/glm.hpp>

#include "engine/export/tessellate/tessellate.h"
#include "spdlog/spdlog.h"

namespace moleculens::engine {

namespace {

#pragma pack(push, 1)
struct StlTriangle {
    float normal[3];
    float v0[3];
    float v1[3];
    float v2[3];
    uint16_t attribute = 0;
};
#pragma pack(pop)

} // namespace

entity::Result<void> StlExporter::exportScene(const SceneCommandList& list,
                                              const std::filesystem::path& stlPath) {
    std::ofstream out(stlPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return entity::Result<void>::error(entity::ErrorCode::IoError,
                                           "Cannot write " + stlPath.string());
    }

    // 计数与实际写出一致：两阶段共用 forEachMeshTriangle
    uint32_t triCount = 0;
    forEachMeshTriangle(list, [&triCount](const glm::vec3&, const glm::vec3&,
                                          const glm::vec3&, uint32_t, uint32_t) {
        ++triCount;
    });

    char header[80] = "MolecuLens STL export";
    out.write(header, 80);
    out.write(reinterpret_cast<const char*>(&triCount), 4);

    forEachMeshTriangle(list, [&out](const glm::vec3& a, const glm::vec3& b,
                                     const glm::vec3& c, uint32_t, uint32_t) {
        // 面法线（归一化叉积）
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        StlTriangle tri;
        tri.normal[0] = n.x;
        tri.normal[1] = n.y;
        tri.normal[2] = n.z;
        tri.v0[0] = a.x; tri.v0[1] = a.y; tri.v0[2] = a.z;
        tri.v1[0] = b.x; tri.v1[1] = b.y; tri.v1[2] = b.z;
        tri.v2[0] = c.x; tri.v2[1] = c.y; tri.v2[2] = c.z;
        out.write(reinterpret_cast<const char*>(&tri), sizeof(StlTriangle));
    });

    spdlog::info("[export] STL written: {} ({} triangles)", stlPath.string(), triCount);
    return entity::Result<void>::ok();
}

} // namespace moleculens::engine
