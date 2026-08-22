// =============================================================================
// MolecuLens · engine · vulkan/offscreen/offscreen_renderer.h —— 离屏渲染
// 规格来源：主方案 §6.1/§12.5（截图/高 DPI 导出/动画导出/CLI 无头；
//           复用同一套命令流执行；readbackPixels RGBA8/16F）
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <volk.h>

#include "engine/cmd/cmd.h"
#include "engine/vulkan/desc/descriptors.h"
#include "engine/vulkan/device/device.h"
#include "engine/vulkan/memory/buffer.h"
#include "engine/vulkan/pipeline/pipeline.h"
#include "engine/vulkan/renderpass/render_pass.h"

namespace moleculens::engine {

struct RenderView;

enum class PixelFormat : uint8_t { Rgba8, Rgba16F };

// 离屏渲染：截图 / 高 DPI 导出 / 动画导出 / CLI 无头（无 Swapchain，主方案 §6.1）
class OffscreenRenderer {
public:
    OffscreenRenderer() = default;
    ~OffscreenRenderer();

    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;
    // 移动也删除：DrawBuffers 不完整类型成员（unique_ptr 析构需在 cpp 实例化）
    OffscreenRenderer(OffscreenRenderer&&) = delete;
    OffscreenRenderer& operator=(OffscreenRenderer&&) = delete;

    // 无窗口初始化（headless，主方案 §11.3 / CLI / GUI 视口 FBO §7.7）
    bool init(const RenderConfig& cfg, uint32_t width, uint32_t height,
              std::string* outError = nullptr);
    void shutdown();

    // 执行命令流到离屏 FBO（MSAA resolve，主方案 §12.5 ③）
    void execute(const SceneCommandList& list, const RenderView& view);

    // 像素回读（vkCmdCopyImage → staging → CPU，主方案 §12.5 ④）
    bool readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt);

    void resize(uint32_t width, uint32_t height);
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    VulkanDevice& device() { return device_; }

    // 颜色附件暴露（ImGui::Image 采样，主方案 §7.7：场景渲染进离屏 FBO）
    VkImageView colorView() const { return colorImage_.view(); }
    VkSampler sampler() const { return sampler_; }
    // 附件布局（execute 后为 SHADER_READ_ONLY_OPTIMAL，可供采样）
    VkImageLayout colorLayout() const { return colorLayout_; }
    void setBackground(const float* rgb);   // 清屏色（主方案 §4.4 背景）

private:
    // 每帧绘制缓冲（完整定义于头文件：OffscreenRenderer 被 app 层 unique_ptr 持有，
    // 跨 TU 析构需完整类型——DrawBuffers 成员均为完整头文件类型）
    struct DrawBuffers {
        Buffer sphereInst;
        Buffer cylinderInst;
        Buffer lineVerts;
        Buffer meshVerts;
        Buffer meshIdx;
        std::vector<SphereInst> spheres;
        std::vector<uint32_t> sphereMat;
        std::vector<CylinderInst> cylinders;
        std::vector<uint32_t> cylinderMat;
        std::vector<LineInst> lines;
        std::vector<uint32_t> lineMat;
        struct Mesh {
            std::vector<TriMeshVertex> verts;
            std::vector<uint32_t> idx;
            uint32_t mat = 0;
        };
        std::vector<Mesh> meshes;
        std::vector<MaterialBlock> materials;
    };

    bool createFramebuffer();
    void destroyFramebuffer();
    template <typename InstT>
    void drawInstancedBatches(VkCommandBuffer cmd,
                              const std::vector<InstT>& instances,
                              const std::vector<uint32_t>& mats,
                              const std::vector<MaterialBlock>& materials,
                              Buffer& instanceBuffer, const Buffer& mesh,
                              const Buffer& meshIdx,
                              uint32_t vertexCount, uint32_t indexCount,
                              VkPipeline pipeline, VkPipelineLayout layout,
                              VkDescriptorSet set);

    VulkanDevice device_;
    RenderConfig config_;
    uint32_t width_ = 1, height_ = 1;
    bool initialized_ = false;

    // 离屏附件
    Image colorImage_;     // 无 MSAA 或 resolve 目标
    Image colorMsaa_;      // MSAA 采样附件
    Image depthImage_;
    RenderPass renderPass_;
    Framebuffer framebuffer_;

    // 管线
    Pipeline spherePipeline_;
    Pipeline cylinderPipeline_;
    Pipeline linePipeline_;
    Pipeline meshPipeline_;
    DescriptorSetLayout globalLayout_;
    UboDescriptorSet globalUbo_;

    // 共享网格（顶点 + 索引）
    std::unique_ptr<Buffer> icosphere_;
    std::unique_ptr<Buffer> icosphereIdx_;
    std::unique_ptr<Buffer> cylinder_;
    std::unique_ptr<Buffer> cylinderIdx_;
    uint32_t icosphereVerts_ = 0, icosphereIndices_ = 0;
    uint32_t cylinderVerts_ = 0, cylinderIndices_ = 0;

    std::unique_ptr<DrawBuffers> drawBuffers_;

    // 回读
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkDeviceSize readbackSize_ = 0;
    std::vector<uint8_t> pixels_;     // 最近一次 readback 结果

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImageLayout colorLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    float clearColor_[4] = {0.05f, 0.05f, 0.08f, 1.0f};
};

} // namespace moleculens::engine
