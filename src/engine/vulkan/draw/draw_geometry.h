// =============================================================================
// MolecuLens · engine · vulkan/draw/draw_geometry.h —— 共享几何绘制
// 规格来源：主方案 §6.4（命令 → GPU 缓冲 → vkCmdDraw*）/ §5.7（各样式）
//
// 职责：把 DrawList（CPU 绘制列表）画进一个已开启的渲染通道。**唯一实现**。
//
// 重构前 VulkanRenderer::recordFrame 与 OffscreenRenderer::execute 各有一份
// 约 180 行的绘制代码（球/键/线段/网格/点 五条路径 + 批次切分），并且：
//   · 两个后端各自重复声明 4 个共享网格 Buffer 成员与一段相同的上传代码；
//   · 两份实现的线段颜色处理**不一致**（见 drawGeometry 内注释），导致
//     视口与导出图的线框亮度不同 —— 属真实缺陷，本次统一到正确一侧。
// =============================================================================

#pragma once

#include <cstdint>

#include <volk.h>

#include "engine/vulkan/backend/draw_list.h"
#include "engine/vulkan/memory/buffer.h"
#include "engine/vulkan/pipeline/pipeline.h"

namespace moleculens::engine {

class VulkanDevice;
struct RenderView;   // 视锥剔除用（定义见 engine/render/renderer.h）

// 共享网格：单位球与单位圆柱的顶点/索引缓冲（引擎启动创建一次常驻）。
// 两个后端原先各自持有一份同构成员并各自实现一遍创建代码。
struct SharedMeshes {
    Buffer   icosphere;
    Buffer   icosphereIdx;
    Buffer   cylinder;
    Buffer   cylinderIdx;
    uint32_t icosphereVertices = 0;
    uint32_t icosphereIndices = 0;
    uint32_t cylinderVertices = 0;
    uint32_t cylinderIndices = 0;

    // detail：球体细分档（1=80 面 … 4=5120 面）；segments：圆柱截面段数。
    bool create(VulkanDevice& device, int detail = 3, int segments = 24);
    void destroy();
};

// 每帧复用的 GPU 缓冲（HOST_VISIBLE；容量按需增长，帧内覆盖写）。
// 各后端各持一份（实时帧槽位与离屏导出互不干扰）。
//
// uploadRevision：缓冲当前内容的来源修订号（命令流 serial）。相机移动、UI 重绘等
// 不改变几何的帧上，绘制列表与缓冲内容完全一致 → 跳过上传（此前每帧无条件重传
// 全部实例数据，静止场景也照传）。
struct FrameGpuBuffers {
    Buffer sphereInst;
    Buffer cylinderInst;
    Buffer lineVerts;
    Buffer meshVerts;
    Buffer meshIdx;
    uint64_t uploadRevision = UINT64_MAX;   // UINT64_MAX = 尚未上传过任何内容

    // 网格（TriMesh）合并后的绘制段：按渲染模式 0..3 各一段，记录该段在
    // meshIdx 缓冲里的起始下标与条数、实际使用的管线模式。
    // 为什么单独缓存：网格是表面/卡通/QuickSurf 的主要载荷（可达数十万顶点），
    // 此前**每帧**都会把全部网格顶点重新拷贝合并一遍（即使 uploadRevision 未变），
    // 静止场景在纯 CPU 上白白复制十几 MB。现在合并与烘焙只在内容变化时做一次，
    // 未变帧直接按缓存的段提交 draw。
    struct MeshDrawRange {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint8_t  pipeMode = 0;
        // 该段第一个网格的材质下标（仅透明段使用：透明网格改用**该表示自己的**
        // 材质参数，而不是固定默认材质 —— 否则 ESP 表面等透明网格的
        // ambient/specular/shininess 设置无效，观感被默认材质刷亮、发灰）。
        uint32_t materialIndex = 0;
    };
    MeshDrawRange meshRanges[4];
    // 上一次网格上传对应的内容修订号。与 listRevision 分开：坐标重放只改坐标命令，
    // 网格字节未变，可复用顶点/索引缓冲，避免每帧重传整张 QuickSurf 表面。
    uint64_t meshUploadRevision = UINT64_MAX;

    // 上一次 drawGeometry 实际提交的绘制调用数（ground truth，每帧开头清零）。
    // 存在的意义：批处理是否生效**必须可观测**。调用数由 computeDrawRuns 的
    // 分段规则决定（见 draw_list.h），段在每个 (材质, 模式) 变化处断开 ——
    // md.gro 曾因"每实例一条材质"而达到 ~4084 次/帧。
    uint32_t drawCalls = 0;

    void destroy();
};

// 把绘制列表画进 cmd（调用方负责已 vkCmdBeginRenderPass、且已设置 viewport/scissor）。
//
// 前置：pipelines 的采样数/renderPass 必须与当前渲染通道一致（见 PipelineSet）。
// 说明：globalSet 为全局 UBO 描述符集（相机/光照/雾）。
//       listRevision 为 draw 的来源命令流 serial：与 gpu.uploadRevision 相同时
//       跳过实例/顶点缓冲重传（内容未变）。
//       meshRevision 为网格内容修订号（SceneCommandList::meshSerial）：坐标重放
//       保持同一值，网格缓冲无需重传；全量重建才变化。
void drawGeometry(VkCommandBuffer cmd, VulkanDevice& device,
                  const PipelineSet& pipelines, const DrawList& draw,
                  FrameGpuBuffers& gpu, const SharedMeshes& meshes,
                  VkDescriptorSet globalSet, uint64_t listRevision,
                  uint64_t meshRevision,
                  const RenderView* view = nullptr);

} // namespace moleculens::engine
