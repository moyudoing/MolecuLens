// =============================================================================
// MolecuLens · engine · vulkan/backend/draw_list.h —— CPU 侧绘制列表
// 规格来源：主方案 §5.3/附录 F（命令流协议）§6.4（命令 → GPU 缓冲编译）
//
// 职责：命令流解码后的**设备无关**中间表示。仅含 POD 与 std::vector，
//       不含任何 Vk* 类型——故实时后端、离屏后端与测试可共用同一份解码结果。
//
// 与 GPU 缓冲的分工：本结构是"CPU 侧要画什么"；各后端另持有自己的
//                     VkBuffer/VMA 资源（"画到哪"），由后端负责上传。
//       重构前 DrawBuffers 把两者混在一个结构体里，并在两个后端各复制一份。
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include "engine/cmd/cmd.h"

namespace moleculens::engine {

struct DrawList {
    // 一个 TriMesh 命令段（卡通 / 表面 / QuickSurf / 导入网格）
    struct Mesh {
        std::vector<TriMeshVertex> verts;
        std::vector<uint32_t>      idx;
        uint32_t                   mat  = 0;   // materials[] 下标
        uint8_t                    mode = 0;   // 体系渲染模式，见 DrawMode
    };

    std::vector<SphereInst>    spheres;
    std::vector<uint32_t>      sphereMat;
    std::vector<uint8_t>       sphereMode;
    std::vector<CylinderInst>  cylinders;
    std::vector<uint32_t>      cylinderMat;
    std::vector<uint8_t>       cylinderMode;
    std::vector<LineInst>      lines;
    std::vector<uint32_t>      lineMat;
    std::vector<Mesh>          meshes;

    // 材质槽。下标 0 恒为默认材质（由 decodeCommandList 置入），
    // 故各几何项以 < 1 的 mat 也总是可安全索引。
    std::vector<MaterialBlock> materials;

    void clear() {
        spheres.clear();
        sphereMat.clear();
        sphereMode.clear();
        cylinders.clear();
        cylinderMat.clear();
        cylinderMode.clear();
        lines.clear();
        lineMat.clear();
        meshes.clear();
        materials.clear();
    }

    [[nodiscard]] bool empty() const {
        return spheres.empty() && cylinders.empty() && lines.empty() && meshes.empty();
    }
};

// 体系渲染模式（Cmd::RenderMode 载荷；决定后端选用的 VkPipeline）
enum class DrawMode : uint8_t {
    Solid     = 0,   // 实心
    Wireframe = 1,   // 线框（polygon LINE）
    Transparent = 2, // 透明（混合）
    Point     = 3,   // 点（GL_POINTS；圆柱/网格降级为线段/实心）
    Count
};

} // namespace moleculens::engine
