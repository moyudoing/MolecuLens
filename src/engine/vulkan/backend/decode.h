// =============================================================================
// MolecuLens · engine · vulkan/backend/decode.h —— 命令流 → CPU 绘制列表
// =============================================================================
// 职责：唯一一份命令流解码实现。重构前该循环在
//       VulkanRenderer::recordFrame 与 OffscreenRenderer::execute 中各有一份
//       （约 100 行 × 2，逐行同构），任何协议扩展都要改两处。
// =============================================================================

#pragma once

#include <cstdint>

namespace moleculens::engine {

class SceneCommandList;
struct DrawList;

// 命令流几何统计（诊断 / 日志用；只做一次线性遍历，不分配 GPU 资源）
struct CommandStats {
    uint32_t spheres = 0;
    uint32_t cylinders = 0;
    uint32_t lines = 0;
    uint32_t meshes = 0;
};
void commandStats(const SceneCommandList& list, CommandStats* out);

// 按 (材质, 体系渲染模式) 解码命令流。
//
// 语义：遍历命令流一次，把 Push 型命令（MaterialBlock / RenderMode）的当前
//       状态**快照**到随后的每条几何项上，使后端能按 (mode, mat) 做连续批次
//       绘制而无须重放命令流。
//
// 容错：载荷长度不足或与元素尺寸不整除的命令被跳过（不中断整帧）。
// 幂等：内部先 out.clear()，再置入下标 0 的默认材质。
void decodeCommandList(const SceneCommandList& list, DrawList& out);

} // namespace moleculens::engine
