// =============================================================================
// MolecuLens · engine · vulkan/device/device.h —— 实例/物理设备/逻辑设备/交换链
// 规格来源：主方案 §6.3（Volk 动态加载；debug + best-practices 校验层；
//           物理设备评分：离散 GPU > 集成；支持 1.2；graphics+compute+present 队列族）
// =============================================================================

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <volk.h>

struct GLFWwindow;

namespace moleculens::engine {

struct RenderConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t msaaSamples = 4;          // 0/2/4/8（0 = 关闭 MSAA）
    bool debug = true;                 // 校验层
    bool vsync = true;
};

// 窗口句柄（GLFW 为主）
using WindowHandle = GLFWwindow*;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    std::optional<uint32_t> compute;
    bool complete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapChainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanDevice {
public:
    VulkanDevice() = default;
    ~VulkanDevice();

    // Volk 初始化 + 实例 + 表面 + 设备（不创建交换链）
    bool init(WindowHandle window, const RenderConfig& cfg, std::string* outError = nullptr);
    // 无窗口（CLI 无头渲染；无 surface/交换链，主方案 §11.3）
    bool initHeadless(const RenderConfig& cfg, std::string* outError = nullptr);
    // 交换链（含深度/MSAA 附件格式查询）
    bool createSwapchain();
    void destroySwapchain();
    bool recreateSwapchain();          // resize/framebuffer 失效时

    [[nodiscard]] VkDevice device() const { return device_; }
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkSurfaceKHR surface() const { return surface_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue presentQueue() const { return presentQueue_; }
    VkQueue computeQueue() const { return computeQueue_; }
    uint32_t graphicsFamily() const { return graphicsFamily_; }
    uint32_t presentFamily() const { return presentFamily_; }

    VkSwapchainKHR swapchain() const { return swapchain_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
    const std::vector<VkImageView>& swapchainViews() const { return swapchainViews_; }
    VkFormat swapchainFormat() const { return swapchainFormat_; }
    VkExtent2D swapchainExtent() const { return swapchainExtent_; }
    const RenderConfig& config() const { return config_; }

    VkFormat depthFormat() const { return VK_FORMAT_D32_SFLOAT; }
    VkSampleCountFlagBits msaaSamples() const { return msaaSamples_; }

    // —— 显存占用（VK_EXT_memory_budget）——
    // 汇总全部 DEVICE_LOCAL 堆的"已用/预算"。这是 Vulkan 里唯一能拿到真实显存
    // 占用的途径（VMA 不暴露分配器，堆总量也无法反映"当前用了多少"）。
    // 扩展是可选启用的（见 device.cpp 的 kOptionalDeviceExtensions）：
    // 未启用时返回 false，调用方应显示"不可用"而不是报 0 —— 0 会被误读成"没占用"。
    struct VramUsage {
        uint64_t usedBytes = 0;
        uint64_t budgetBytes = 0;
        uint32_t heapCount = 0;
    };
    [[nodiscard]] bool queryVramUsage(VramUsage* out) const;
    // 扩展是否已启用（未启用时 queryVramUsage 恒返回 false）
    [[nodiscard]] bool hasMemoryBudget() const { return memoryBudgetEnabled_; }

    VkCommandPool commandPool() const { return commandPool_; }
    // 每帧命令缓冲
    VkCommandBuffer beginFrameCommands();
    void endFrameCommands(VkCommandBuffer cmd);

    uint32_t acquireNextImage(VkSemaphore signal, uint32_t* outIndex);
    VkResult presentImage(uint32_t imageIndex, VkSemaphore wait);

    // 资源辅助
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer cmd);

    // 队列提交互斥：后台导出/导入线程与主线程共用同一 graphics 队列时，
    // 每次 vkQueueSubmit 前须持锁（Vulkan 要求队列提交外部同步）。
    // 调用方在 beginSingleTimeCommands/endSingleTimeCommands 或 endFrame 提交前持锁。
    std::unique_lock<std::mutex> lockQueue() {
        return std::unique_lock<std::mutex>(queueMutex_);
    }
    void createImage(VkFormat format, VkExtent2D extent, VkSampleCountFlagBits samples,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkImage* outImage, VkDeviceMemory* outMemory);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                uint32_t mipLevels = 1);

    // 同步对象（帧循环）
    const std::vector<VkSemaphore>& imageAvailable() const { return imageAvailable_; }
    const std::vector<VkSemaphore>& renderFinished() const { return renderFinished_; }
    const std::vector<VkFence>& inFlightFences() const { return inFlightFences_; }
    size_t maxFramesInFlight() const { return kMaxFramesInFlight; }

    // 校验层状态
    bool validationEnabled() const { return validationEnabled_; }

private:
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createCommandPool();
    bool createSyncObjects();
    VkSurfaceFormatKHR chooseSurfaceFormat() const;
    VkPresentModeKHR choosePresentMode() const;
    VkExtent2D chooseExtent() const;
    void cleanupSwapchain();

    static constexpr size_t kMaxFramesInFlight = 2;

    WindowHandle window_ = nullptr;
    RenderConfig config_;
    bool validationEnabled_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    uint32_t presentFamily_ = 0;
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    bool memoryBudgetEnabled_ = false;   // VK_EXT_memory_budget 是否已启用

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D swapchainExtent_{};

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence> inFlightFences_;
    std::mutex queueMutex_;   // 队列提交互斥（见 lockQueue）
};

} // namespace moleculens::engine
