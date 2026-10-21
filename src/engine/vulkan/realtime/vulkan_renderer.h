// =============================================================================
// MolecuLens · engine · vulkan/realtime/vulkan_renderer.h —— 唯一渲染后端（窗口实时）
// 规格来源：主方案 §6.1/§6.3/§6.4（帧循环两阶段；命令流 → GPU 缓冲编译（serial
//           缓存键 P5）；MSAA；ImGui 经 UI 回调注入（engine 不依赖 imgui））
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <volk.h>

#include "engine/cmd/cmd.h"
#include "engine/vulkan/backend/draw_list.h"
#include "engine/vulkan/desc/descriptors.h"
#include "engine/vulkan/device/device.h"
#include "engine/vulkan/draw/draw_geometry.h"
#include "engine/vulkan/memory/buffer.h"
#include "engine/vulkan/pipeline/pipeline.h"
#include "engine/vulkan/renderpass/render_pass.h"
#include "engine/vulkan/shadow/shadow_map.h"

namespace moleculens::engine {

struct RenderView;
class SceneCommandList;
class ShaderModule;

// 唯一渲染后端：窗口实时渲染（主方案 §6.1）
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    // 实例/设备/交换链/管线/共享网格（主方案 §6.3 初始化流程）
    bool init(WindowHandle window, const RenderConfig& cfg,
              std::string* outError = nullptr);
    void shutdown();

    // —— 帧循环（主方案 §6.3 两阶段）——
    void beginFrame();
    // Stage2：命令流 → GPU 缓冲 → 绘制（含 UI 回调；view 提供相机/视口）
    void execute(const SceneCommandList& list, const RenderView& view);
    void endFrame();
    void resize(uint32_t w, uint32_t h);
    bool windowResized() const { return resized_; }
    void clearResized() { resized_ = false; }

    // —— ImGui 集成（app 层注入；engine 不依赖 imgui）——
    using UiRenderFn = void (*)(VkCommandBuffer);
    void setUiRenderCallback(UiRenderFn fn) { uiRenderFn_ = fn; }

    // —— 资源暴露（app 层 ImGui 后端初始化用）——
    VulkanDevice& device() { return device_; }
    VkRenderPass mainRenderPass() const { return mainRenderPass_.renderPass(); }
    VkDescriptorPool imGuiPool() const { return imguiPool_; }
    VkSampleCountFlagBits msaaSamples() const { return device_.msaaSamples(); }
    VkExtent2D swapchainExtent() const { return device_.swapchainExtent(); }
    uint32_t frameIndex() const { return frameIndex_; }
    VkCommandBuffer frameCommand() const { return frameCommand_; }

    // 拾取 ID Pass 支持（M2）
    bool createPickResources();
    void destroyPickResources();
    bool pickPassEnabled() const { return pickResources_ != nullptr; }

private:
    // M2 拾取资源（完整定义：VulkanRenderer 被 app 层持有，跨 TU 析构需完整类型）
    struct PickResources {
        // M2 落地：离屏 R32_UINT FBO + pick 管线
    };
    bool createPipelines();
    void createImGuiPool();
    bool createMainFramebuffers();      // MSAA 附件 + 深度 + 每交换链图像 framebuffer
    void destroyMainFramebuffers();

    VulkanDevice device_;
    RenderConfig config_;

    // 标准管线集（10 条管线 + 着色器模块；见 vulkan/pipeline/pipeline.h）
    PipelineSet pipelines_;

    DescriptorSetLayout globalLayout_;
    UboDescriptorSet globalUbo_;
    RenderPass mainRenderPass_;

    // 主渲染附件与帧缓冲（MSAA resolve + 深度；每交换链图像一个 framebuffer）
    Image msaaImage_;
    Image depthImage_;
    std::vector<Framebuffer> swapchainFramebuffers_;

    // 方向光阴影（深度 pass + PCF；深度图经 set 0 binding 1 供 frag 采样）
    ShadowMap       shadow_;

    // 共享网格与每帧绘制状态（定义见 vulkan/draw/draw_geometry.h）
    SharedMeshes    meshes_;     // 球/柱单位网格（启动创建一次常驻）
    FrameGpuBuffers gpu_;        // 每帧 GPU 缓冲（HOST_VISIBLE，帧内复用）
    DrawList        drawList_;   // 每帧 CPU 绘制列表（解码结果）
    // 解码缓存键：命令流对象 + serial。serial 未变即内容未变 → 跳过整条重解码
    //（serial 在命令流重建/坐标重放时递增，见 SceneCommandList）。
    const SceneCommandList* decodedList_ = nullptr;
    uint64_t          decodedRevision_ = UINT64_MAX;

    // 帧状态
    uint32_t frameIndex_ = 0;
    VkCommandBuffer frameCommand_ = VK_NULL_HANDLE;
    // 每帧槽位命令缓冲（kMaxFramesInFlight 个）：endFrame 不释放，
    // 下一帧该槽位 fence 等待后释放，避免 vkQueueSubmit 提交已释放句柄。
    // 槽位数与 device 的 kMaxFramesInFlight(=2) 一致
    VkCommandBuffer frameCommandSlots_[2] = {};
    uint32_t imageIndex_ = 0;
    bool resized_ = false;
    bool initialized_ = false;

    // ImGui
    VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
    UiRenderFn uiRenderFn_ = nullptr;

    // 拾取
    std::unique_ptr<PickResources> pickResources_;

    void recordFrame(const SceneCommandList& list, const RenderView& view);
};

} // namespace moleculens::engine
