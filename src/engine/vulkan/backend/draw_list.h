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

// 一次实例化绘制：材质与渲染模式相同的**连续**实例段
struct DrawRun {
    uint32_t start = 0;    // 实例数组起始下标
    uint32_t count = 0;    // 实例数
    uint32_t mat   = 0;    // materials[] 下标
    uint8_t  mode  = 0;    // DrawMode
};

// 把"材质下标与渲染模式相同的连续实例"合并为一次绘制。
//
// **单一事实来源**：后端按它提交绘制（drawModeRuns），绘制调用数就由它决定。
// 段在某处断开 → 那里就是一次额外的 `vkCmdDrawIndexed`。因此本函数是
// "批处理是否生效"的唯一可断言对象（纯函数，无 GPU 也能测）。
//
// 历史缺陷（本函数被抽出来的原因）：`Representer::generate` 曾**每个实例**发一条
// `MaterialBlock`，而解码端每条都新增材质槽 → 2042 个内容相同的材质 → 2042 个
// 不同下标 → 段在每个实例边界断开，实测 md.gro 单帧 ~4084 次绘制、平均每次仅
// 3.2 个实例。材质改为每表示发一条后，同表示的实例共用一个下标，段自然合并。
//
// count 显式传入：调用方的实例数与 mats/modes 长度理论上一致，但越界时按
// 下标的默认值 (mat 0 / mode 0) 处理（与重构前的 `mats.empty() ? 0 : mats[i]`
// 行为一致），不因数组长度不齐而少画或多画。
inline std::vector<DrawRun> computeDrawRuns(const std::vector<uint32_t>& mats,
                                            const std::vector<uint8_t>& modes,
                                            size_t count) {
    std::vector<DrawRun> runs;
    if (count == 0) {
        return runs;
    }
    const auto matAt = [&mats](size_t i) -> uint32_t {
        return i < mats.size() ? mats[i] : 0u;
    };
    const auto modeAt = [&modes](size_t i) -> uint8_t {
        return i < modes.size() ? modes[i] : uint8_t{0};
    };
    uint32_t start = 0;
    uint32_t curMat = matAt(0);
    uint8_t curMode = modeAt(0);
    for (size_t i = 1; i < count; ++i) {
        const uint32_t m = matAt(i);
        const uint8_t md = modeAt(i);
        if (m != curMat || md != curMode) {
            runs.push_back(DrawRun{start, static_cast<uint32_t>(i - start), curMat, curMode});
            start = static_cast<uint32_t>(i);
            curMat = m;
            curMode = md;
        }
    }
    runs.push_back(
        DrawRun{start, static_cast<uint32_t>(count - start), curMat, curMode});
    return runs;
}

} // namespace moleculens::engine
