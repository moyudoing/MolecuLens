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

    bool create(VulkanDevice& device);
    void destroy();
};

// 每帧复用的 GPU 缓冲（HOST_VISIBLE；容量按需增长，帧内覆盖写）。
// 各后端各持一份（实时帧槽位与离屏导出互不干扰）。
struct FrameGpuBuffers {
    Buffer sphereInst;
    Buffer cylinderInst;
    Buffer lineVerts;
    Buffer meshVerts;
    Buffer meshIdx;

    void destroy();
};

// 把绘制列表画进 cmd（调用方负责已 vkCmdBeginRenderPass、且已设置 viewport/scissor）。
//
// 前置：pipelines 的采样数/renderPass 必须与当前渲染通道一致（见 PipelineSet）。
// 说明：globalSet 为全局 UBO 描述符集（相机/光照/雾）。
void drawGeometry(VkCommandBuffer cmd, VulkanDevice& device,
                  const PipelineSet& pipelines, const DrawList& draw,
                  FrameGpuBuffers& gpu, const SharedMeshes& meshes,
                  VkDescriptorSet globalSet);

} // namespace moleculens::engine
