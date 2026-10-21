// =============================================================================
// MolecuLens · engine · export/tessellate/tessellate.h —— 命令流 → 三角形汤
// 规格来源：主方案 §6.2（球 → 细分 icosphere；圆柱 → 8 段网格；TriMesh 直出）
//
// 职责：把设备无关命令流里的**实例化图元**（SphereInst / CylinderInst）连同
//       TriMesh，统一展开为世界空间三角形，供 OBJ / STL 等"只认三角形"的
//       导出格式使用。
//
// 为什么需要它：STL 此前只处理 TriMesh，于是球棍/空间填充这类以球实例为主的
//       表示会静默导出**空文件**；OBJ 则自带一份展开实现。把展开规则收敛到
//       这里，两种格式共用同一份几何，导出的三角形也与屏幕上渲染的一致
//       （单位球取自 vulkan/geometry 的同一实现：细分 2 级 = 320 三角形）。
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>

#include "engine/cmd/cmd.h"
#include "engine/vulkan/geometry/geometry.h"

namespace moleculens::engine {

// 遍历命令流中所有可三角化的几何，对每个三角形调用
//   fn(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
//      uint32_t objectId, uint32_t materialIndex)
//   · objectId 在每个图元边界递增（每个球 / 每个圆柱 / 每个 TriMesh 段各占一个），
//     供 OBJ 生成唯一的 `o` 对象名；不需要时忽略即可。
//   · materialIndex = 已出现的 MaterialBlock 命令条数（无材质时为 0），
//     与命令流中材质块的先后顺序一一对应，供 OBJ 写 usemtl。
//   · 载荷长度不足 / 不整除 / 索引越界的部分被跳过（不读越界内存）。
template <typename Fn>
void forEachMeshTriangle(const SceneCommandList& list, Fn&& fn) {
    // 单位网格（与渲染共用同一实现）
    std::vector<glm::vec3> sphereVerts, cylVerts;
    std::vector<uint32_t> sphereIdx, cylIdx;
    generateIcosphere(sphereVerts, sphereIdx);
    generateCylinder(cylVerts, cylIdx);

    uint32_t objectId = 0;
    uint32_t currentMaterial = 0;   // 已出现的材质块数（与命令流顺序一致）
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint8_t* p = it.payload();
        const uint32_t size = it.payloadSize();
        switch (it.code()) {
            case Cmd::MaterialBlock: {
                if (size >= sizeof(MaterialBlock)) {
                    ++currentMaterial;
                }
                break;
            }
            case Cmd::SphereInst: {
                if (size < sizeof(SphereInst) || size % sizeof(SphereInst) != 0) {
                    break;
                }
                const uint32_t n = size / sizeof(SphereInst);
                for (uint32_t i = 0; i < n; ++i) {
                    SphereInst inst{};
                    std::memcpy(&inst, p + i * sizeof(SphereInst), sizeof(SphereInst));
                    const glm::vec3 c = glm::vec3(inst.posRadius);
                    const float r = inst.posRadius.w;
                    ++objectId;
                    for (size_t k = 0; k + 2 < sphereIdx.size(); k += 3) {
                        fn(c + sphereVerts[sphereIdx[k]] * r,
                           c + sphereVerts[sphereIdx[k + 1]] * r,
                           c + sphereVerts[sphereIdx[k + 2]] * r, objectId,
                           currentMaterial);
                    }
                }
                break;
            }
            case Cmd::CylinderInst: {
                if (size < sizeof(CylinderInst) || size % sizeof(CylinderInst) != 0) {
                    break;
                }
                const uint32_t n = size / sizeof(CylinderInst);
                for (uint32_t i = 0; i < n; ++i) {
                    CylinderInst inst{};
                    std::memcpy(&inst, p + i * sizeof(CylinderInst), sizeof(CylinderInst));
                    const glm::vec3 a = glm::vec3(inst.aPos), b = glm::vec3(inst.bPos);
                    const glm::vec3 axis = b - a;
                    const float halfLen = glm::length(axis) * 0.5f;
                    const glm::vec3 dir =
                        halfLen > 1e-6f ? axis / (2.0f * halfLen) : glm::vec3(0, 1, 0);
                    const glm::vec3 up =
                        std::fabs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                    const glm::vec3 right = glm::normalize(glm::cross(up, dir));
                    const glm::vec3 up2 = glm::cross(dir, right);
                    ++objectId;
                    for (size_t k = 0; k + 2 < cylIdx.size(); k += 3) {
                        const auto place = [&](uint32_t vi) {
                            const glm::vec3& v = cylVerts[vi];
                            return a + dir * (v.y * halfLen) + right * (v.x * inst.radius) +
                                   up2 * (v.z * inst.radius);
                        };
                        fn(place(cylIdx[k]), place(cylIdx[k + 1]), place(cylIdx[k + 2]),
                           objectId, currentMaterial);
                    }
                }
                break;
            }
            case Cmd::TriMesh: {
                if (size < sizeof(TriMeshHeader)) {
                    break;
                }
                TriMeshHeader hdr{};
                std::memcpy(&hdr, p, sizeof(TriMeshHeader));
                const uint32_t vertBytes = hdr.vertexCount * sizeof(TriMeshVertex);
                const uint32_t idxBytes = hdr.indexCount * sizeof(uint32_t);
                if (sizeof(TriMeshHeader) + vertBytes + idxBytes > size) {
                    break;
                }
                ++objectId;
                const uint8_t* vp = p + sizeof(TriMeshHeader);
                const uint8_t* ip = vp + vertBytes;
                const auto vertexAt = [&](uint32_t vi) {
                    TriMeshVertex v{};
                    std::memcpy(&v, vp + vi * sizeof(TriMeshVertex), sizeof(TriMeshVertex));
                    return glm::vec3(v.pos);
                };
                for (uint32_t t = 0; t + 2 < hdr.indexCount; t += 3) {
                    uint32_t ia = 0, ib = 0, ic = 0;
                    std::memcpy(&ia, ip + t * sizeof(uint32_t), sizeof(uint32_t));
                    std::memcpy(&ib, ip + (t + 1) * sizeof(uint32_t), sizeof(uint32_t));
                    std::memcpy(&ic, ip + (t + 2) * sizeof(uint32_t), sizeof(uint32_t));
                    if (ia >= hdr.vertexCount || ib >= hdr.vertexCount ||
                        ic >= hdr.vertexCount) {
                        continue;
                    }
                    fn(vertexAt(ia), vertexAt(ib), vertexAt(ic), objectId, currentMaterial);
                }
                break;
            }
            default:
                break;
        }
    }
}

} // namespace moleculens::engine
