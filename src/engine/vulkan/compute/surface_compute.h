// =============================================================================
// MolecuLens · engine · vulkan/compute/surface_compute.h —— 表面 GPU 计算（Vulkan 计算着色器）
// 规格来源：主方案 §5.7.6 vdW 表面 + style/quicksurf 高斯表面（GPU 加速实现）
// 功能：把 Surface（原子 vdW 球面最小距离场）/ QuickSurf（高斯密度场 + 梯度 +
//       密度加权色）的密度体素计算移到 GPU 计算着色器，CPU 仅做 Marching Cubes
//       抽取等值面 → TriMesh。
// 计算管线自行构建（不改造图形 PipelineBuilder）：compute 管线 + 描述符集 +
//   SSBO（atoms / cell 网格 / 输出 density/grad/colormap）+ push constant + dispatch。
// 队列提交全程持 VulkanDevice::lockQueue()。
// 不可用/创建失败 → 上层回退 CPU（surfaceSetBackend 开关），不阻塞渲染。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <volk.h>

#include "engine/vulkan/device/device.h"
#include "engine/vulkan/memory/buffer.h"

namespace moleculens::engine {

class VulkanDevice;

// 计算着色器 push constant（128B；与 shaders/surface.comp 的 PushCtl 一一对应。
// 全部成员为 16B（vec4/ivec4），C++ 与 GLSL 布局一一映射，无对齐风险）
struct SurfaceCompParams {
    glm::ivec4 grid;        // x,y,z = 体素网格维度(Nx,Ny,Nz)
    glm::vec4  origin;      // xyz = 体素网格原点(Å)
    glm::vec4  invSpacing;  // xyz = 1/网格间距
    glm::ivec4 mode;        // x = 模式(0 Surface / 1 QuickSurf), y = hasColor
    glm::vec4  params2;     // x = isovalue, z = gausslim
    glm::ivec4 cellDims;    // x,y,z = cell 网格维度
    glm::vec4  cellOrigin;  // xyz = cell 网格原点(Å)
    glm::vec4  cellSize;    // x = cell 边长(Å)
};
static_assert(sizeof(SurfaceCompParams) == 128, "SurfaceCompParams must be 128B");

// Surface/QuickSurf 的 GPU 计算输出（CPU 端 MC 抽取输入）
struct SurfaceFieldResult {
    std::vector<float>   density;    // [nvox]
    std::vector<float>   gradient;   // [nvox*3]（模式 1 填充）
    std::vector<float>   colormap;   // [nvox*3]（模式 1 且 hasColor）
};

// 原子输入（xyz Å + vdW 半径 Å；r 已乘 radscale 的最终半径）
struct SurfaceAtom {
    glm::vec3 pos;
    float     radius;
};

// cell 网格输入（构网参数；cell 边长 = 覆盖半径上限）
struct SurfaceGrid {
    glm::vec3      origin;    // cell 网格原点(Å)
    glm::ivec3     dims;      // cell 网格维度
    float          cellSize;  // cell 边长(Å)
    uint32_t       nCells;
    std::vector<uint32_t> cellOffsets;  // [nCells+1]（前缀和起点）
    std::vector<uint32_t> cellCounts;   // [nCells]
    std::vector<uint32_t> cellList;     // 稠密原子下标
};

class SurfaceCompute {
public:
    SurfaceCompute() = default;
    ~SurfaceCompute();
    SurfaceCompute(const SurfaceCompute&) = delete;
    SurfaceCompute& operator=(const SurfaceCompute&) = delete;

    // 构建 compute 管线 + SSBO（失败返回 false；上层回退 CPU）
    bool create(VulkanDevice& device);

    // 计算字段：centers/radii(Å, r 已乘 radscale)、colors(RGB, 可空)、mode、
    // isovalue、gridspacing、voxOrigin/voxDims。写入 out。
    bool compute(const std::vector<SurfaceAtom>& atoms,
                 const std::vector<glm::vec3>*   colors,   // 可选 RGB
                 int mode, float isovalue, float gridspacing,
                 const glm::vec3& voxOrigin, const glm::ivec3& voxDims,
                 SurfaceFieldResult& out);

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

    void destroy();

    // 供上层检查当前 GPU 支持状态（后端开关显示）
    static bool isAvailableOn(VulkanDevice& device);

private:
    bool buildGrid(const std::vector<SurfaceAtom>& atoms,
                   float gridSpacing, SurfaceGrid& grid);
    bool ensureBuffers(int nAtoms, uint32_t nCells, int nVox, int nVox3, bool hasColor);

    VulkanDevice* device_ = nullptr;

    // 计算管线资源
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool       pool_ = VK_NULL_HANDLE;
    VkDescriptorSet        set_ = VK_NULL_HANDLE;
    VkPipelineLayout       pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline             pipeline_ = VK_NULL_HANDLE;
    VkShaderModule         shaderModule_ = VK_NULL_HANDLE;

    // SSBO
    std::unique_ptr<Buffer> atomsBuf_;       // [nAtoms*16]
    std::unique_ptr<Buffer> cellOffBuf_;     // [nCells*4]
    std::unique_ptr<Buffer> cellCntBuf_;     // [nCells*4]
    std::unique_ptr<Buffer> cellListBuf_;    // [nAtoms*4]
    std::unique_ptr<Buffer> colorBuf_;       // [nAtoms*16]（RGBA）
    std::unique_ptr<Buffer> densBuf_;        // 回读 HOST_VISIBLE
    std::unique_ptr<Buffer> gradBuf_;
    std::unique_ptr<Buffer> colmapBuf_;

    uint32_t capAtoms_ = 0;
    uint32_t capCells_ = 0;
    uint32_t capVox_ = 0;
    uint32_t capVox3_ = 0;
};

} // namespace moleculens::engine