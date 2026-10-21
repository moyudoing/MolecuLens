// =============================================================================
// MolecuLens · engine · export/obj/obj_exporter.cpp —— OBJ 导出实现
// 规格来源：主方案 §6.2 / §12.4（遍历场景几何写文件）
//
// 几何来源：export/tessellate 的共享展开规则（球/圆柱由单位网格实例化展开，
//           TriMesh 直出），与 STL 导出器、以及屏幕渲染共用同一份单位网格
//           （vulkan/geometry，细分 2 级 = 320 三角形），故导出模型与所见一致。
//
// 输出形态：每个图元一个 `o mesh_<n>`；三角形独立展开（每面 3 个顶点）。
//           顶点不跨面复用——换来的是实现简单、且与 tessellate 的流式回调契合；
//           OBJ 属交换/调试格式，体积换简洁在此可接受。
// =============================================================================

#include "engine/export/obj/obj_exporter.h"

#include <cstring>
#include <fstream>

#include "engine/export/tessellate/tessellate.h"
#include "spdlog/spdlog.h"

namespace moleculens::engine {

namespace {

// 写一条 MTL 材质定义
void writeMtl(std::ostream& outMtl, const MaterialBlock& m) {
    outMtl << "Kd " << m.diffuse.r * m.diffuse.w << " " << m.diffuse.g * m.diffuse.w
           << " " << m.diffuse.b * m.diffuse.w << "\n";
    outMtl << "Ka " << m.ambient.r << " " << m.ambient.g << " " << m.ambient.b << "\n";
    outMtl << "Ks " << m.specular.r << " " << m.specular.g << " " << m.specular.b << "\n";
    outMtl << "Ns " << m.shininess << "\n";
    outMtl << "d " << m.opacity << "\n\n";
}

} // namespace

entity::Result<void> ObjExporter::exportToStream(const SceneCommandList& list,
                                                 std::ostream& outObj,
                                                 std::ostream& outMtl) {
    // —— 材质表：mat_0 恒为默认材质，其后每个 MaterialBlock 命令依次 mat_1..mat_N。
    //    编号与 tessellate 回调里的 materialIndex 一一对应。 ——
    outMtl << "newmtl mat_0\n";
    writeMtl(outMtl, MaterialBlock{});
    {
        uint32_t k = 0;
        CommandIterator it(list);
        for (bool has = list.first(it); has; has = list.next(it)) {
            if (it.code() != Cmd::MaterialBlock ||
                it.payloadSize() < sizeof(MaterialBlock)) {
                continue;
            }
            MaterialBlock mat{};
            std::memcpy(&mat, it.payload(), sizeof(MaterialBlock));
            outMtl << "newmtl mat_" << ++k << "\n";
            writeMtl(outMtl, mat);
        }
    }

    // —— 几何：共享展开规则（球 / 圆柱 / TriMesh） ——
    uint32_t baseVertex = 1;        // OBJ 顶点从 1 开始
    uint32_t lastObject = 0;
    uint32_t lastMaterial = 0;
    forEachMeshTriangle(
        list,
        [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
            uint32_t objectId, uint32_t materialIndex) {
            if (objectId != lastObject) {
                outObj << "o mesh_" << objectId << "\n";
                lastObject = objectId;
            }
            if (materialIndex != lastMaterial) {
                outObj << "usemtl mat_" << materialIndex << "\n";
                lastMaterial = materialIndex;
            }
            outObj << "v " << a.x << " " << a.y << " " << a.z << "\n";
            outObj << "v " << b.x << " " << b.y << " " << b.z << "\n";
            outObj << "v " << c.x << " " << c.y << " " << c.z << "\n";
            outObj << "f " << baseVertex << " " << baseVertex + 1 << " "
                   << baseVertex + 2 << "\n";
            baseVertex += 3;
        });

    return entity::Result<void>::ok();
}

entity::Result<void> ObjExporter::exportScene(const SceneCommandList& list,
                                              const std::filesystem::path& objPath) {
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    if (!obj) {
        return entity::Result<void>::error(entity::ErrorCode::IoError,
                                           "Cannot write " + objPath.string());
    }
    auto mtlPath = objPath;
    mtlPath.replace_extension(".mtl");
    std::ofstream mtl(mtlPath, std::ios::binary | std::ios::trunc);
    if (!mtl) {
        return entity::Result<void>::error(entity::ErrorCode::IoError,
                                           "Cannot write " + mtlPath.string());
    }
    obj << "# MolecuLens OBJ export\n";
    obj << "mtllib " << mtlPath.filename().string() << "\n";
    const auto result = exportToStream(list, obj, mtl);
    if (result) {
        spdlog::info("[export] OBJ written: {}", objPath.string());
    }
    return result;
}

} // namespace moleculens::engine
