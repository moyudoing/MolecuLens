// =============================================================================
// MolecuLens · engine · export/pov/pov_exporter.cpp —— POV-Ray 导出实现
// 规格来源：主方案 §6.2（球/圆柱 → POV 图元；材质 → finish/color）
// =============================================================================

#include "engine/export/pov/pov_exporter.h"

#include <cstring>
#include <glm/glm.hpp>
#include <fstream>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

entity::Result<void> PovExporter::exportScene(const SceneCommandList& list,
                                              const std::filesystem::path& povPath) {
    std::ofstream out(povPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return entity::Result<void>::error(entity::ErrorCode::IoError,
                                           "Cannot write " + povPath.string());
    }
    out << "// MolecuLens POV-Ray export\n";
    out << "#version 3.7;\n";
    out << "global_settings { assumed_gamma 1.0 }\n";
    out << "camera { location <40, 30, 40> look_at <0, 0, 0> }\n";
    out << "light_source { <100, 100, 100> color rgb <1,1,1> }\n";
    out << "background { color rgb <0.05,0.05,0.08> }\n\n";

    MaterialBlock curMat{};
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint8_t* p = it.payload();
        switch (it.code()) {
            case Cmd::MaterialBlock: {
                if (it.payloadSize() >= sizeof(MaterialBlock)) {
                    std::memcpy(&curMat, p, sizeof(MaterialBlock));
                }
                break;
            }
            case Cmd::SphereInst: {
                if (it.payloadSize() >= sizeof(SphereInst)) {
                    const uint32_t n = it.payloadSize() / sizeof(SphereInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        const auto& inst =
                            *reinterpret_cast<const SphereInst*>(p + i * sizeof(SphereInst));
                        const auto c = inst.color * curMat.diffuse;
                        out << "sphere { <" << inst.posRadius.x << ", "
                            << inst.posRadius.y << ", " << inst.posRadius.z << ">, "
                            << inst.posRadius.w << "\n";
                        out << "  pigment { color rgb <" << c.r << ", " << c.g << ", "
                            << c.b << "> transmit " << (1.0f - curMat.opacity) << " }\n";
                        out << "  finish { phong 0.6 diffuse 0.8 }\n}\n";
                    }
                }
                break;
            }
            case Cmd::CylinderInst: {
                if (it.payloadSize() >= sizeof(CylinderInst)) {
                    const uint32_t n = it.payloadSize() / sizeof(CylinderInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        const auto& inst =
                            *reinterpret_cast<const CylinderInst*>(p + i * sizeof(CylinderInst));
                        const auto c = inst.color * curMat.diffuse;
                        out << "cylinder { <" << inst.aPos.x << ", " << inst.aPos.y
                            << ", " << inst.aPos.z << ">, <" << inst.bPos.x << ", "
                            << inst.bPos.y << ", " << inst.bPos.z << ">, "
                            << inst.radius << "\n";
                        out << "  pigment { color rgb <" << c.r << ", " << c.g << ", "
                            << c.b << "> }\n";
                        out << "  finish { phong 0.6 diffuse 0.8 }\n}\n";
                    }
                }
                break;
            }
            case Cmd::LineInst: {
                // POV-Ray 无线段图元 → 用细圆柱表示（线框/坐标轴/点模式键走的都是它）
                constexpr float kLineRadius = 0.03f;
                if (it.payloadSize() >= sizeof(LineInst) &&
                    it.payloadSize() % sizeof(LineInst) == 0) {
                    const uint32_t n = it.payloadSize() / sizeof(LineInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        LineInst inst{};
                        std::memcpy(&inst, p + i * sizeof(LineInst), sizeof(LineInst));
                        const auto c = glm::vec4(inst.color) * glm::vec4(curMat.diffuse);
                        out << "cylinder { <" << inst.aPos.x << ", " << inst.aPos.y << ", "
                            << inst.aPos.z << ">, <" << inst.bPos.x << ", " << inst.bPos.y
                            << ", " << inst.bPos.z << ">, " << kLineRadius << "\n";
                        out << "  pigment { color rgb <" << c.r << ", " << c.g << ", " << c.b
                            << "> }\n";
                        out << "  finish { phong 0.6 diffuse 0.8 }\n}\n";
                    }
                }
                break;
            }
            case Cmd::TriMesh: {
                // 三角网格 → mesh2（卡通/表面/QuickSurf/导入网格）。
                // 注：mesh2 不支持逐顶点色（需 texture_list + face 纹理索引）；
                //     此处用当前材质着色，故表面/卡通的二级结构配色不会体现在 POV 中。
                if (it.payloadSize() >= sizeof(TriMeshHeader)) {
                    TriMeshHeader hdr{};
                    std::memcpy(&hdr, p, sizeof(TriMeshHeader));
                    const uint32_t vertBytes = hdr.vertexCount * sizeof(TriMeshVertex);
                    const uint32_t idxBytes = hdr.indexCount * sizeof(uint32_t);
                    if (hdr.vertexCount > 0 && hdr.indexCount >= 3 &&
                        sizeof(TriMeshHeader) + vertBytes + idxBytes <= it.payloadSize()) {
                        const uint8_t* vp = p + sizeof(TriMeshHeader);
                        const uint8_t* ip = vp + vertBytes;
                        out << "mesh2 {\n  vertex_vectors {\n    " << hdr.vertexCount;
                        for (uint32_t i = 0; i < hdr.vertexCount; ++i) {
                            TriMeshVertex v{};
                            std::memcpy(&v, vp + i * sizeof(TriMeshVertex),
                                        sizeof(TriMeshVertex));
                            out << ",\n    <" << v.pos.x << ", " << v.pos.y << ", " << v.pos.z
                                << ">";
                        }
                        out << "\n  }\n  face_indices {\n    " << (hdr.indexCount / 3);
                        for (uint32_t i = 0; i + 2 < hdr.indexCount; i += 3) {
                            uint32_t ia = 0, ib = 0, ic = 0;
                            std::memcpy(&ia, ip + i * sizeof(uint32_t), sizeof(uint32_t));
                            std::memcpy(&ib, ip + (i + 1) * sizeof(uint32_t), sizeof(uint32_t));
                            std::memcpy(&ic, ip + (i + 2) * sizeof(uint32_t), sizeof(uint32_t));
                            if (ia >= hdr.vertexCount || ib >= hdr.vertexCount ||
                                ic >= hdr.vertexCount) {
                                continue;
                            }
                            out << ",\n    <" << ia << ", " << ib << ", " << ic << ">";
                        }
                        out << "\n  }\n";
                        out << "  pigment { color rgb <" << curMat.diffuse.r << ", "
                            << curMat.diffuse.g << ", " << curMat.diffuse.b << "> transmit "
                            << (1.0f - curMat.opacity) << " }\n";
                        out << "  finish { phong 0.6 diffuse 0.8 }\n}\n";
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    spdlog::info("[export] POV written: {}", povPath.string());
    return entity::Result<void>::ok();
}

} // namespace moleculens::engine
