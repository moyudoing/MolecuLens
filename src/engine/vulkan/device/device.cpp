// =============================================================================
// MolecuLens · engine · vulkan/device/device.cpp —— Vulkan 设备实现
// 规格来源：主方案 §6.3（Volk；校验层 debug + best-practices；设备评分；
//           MSAA 采样数选择；交换链）
// =============================================================================

#include "engine/vulkan/device/device.h"

#include <algorithm>
#include <cstring>
#include <set>

#include <GLFW/glfw3.h>

#include "spdlog/spdlog.h"

#include "engine/cmd/cmd.h"

#include <volk.h>

namespace moleculens::engine {

namespace {

// 校验层（主方案 §6.3：debug + best-practices）
const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation",
};
// 调试消息级别
const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    (void)type; (void)user;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        spdlog::error("[backend] Vulkan validation: {}", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        spdlog::warn("[backend] Vulkan validation: {}", data->pMessage);
    } else {
        spdlog::debug("[backend] Vulkan validation: {}", data->pMessage);
    }
    return VK_FALSE;
}

// 设备评分（主方案 §6.3：离散 GPU > 集成；支持 1.2）
int rateDevice(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 100;
    }
    score += static_cast<int>(props.limits.maxImageDimension2D / 1000);

    // Vulkan 1.2 支持（主方案 §6.3）
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        score += 500;
    }
    if (!features.geometryShader) {
        return 0;
    }
    return score;
}

} // namespace

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    cleanupSwapchain();
    for (auto fence : inFlightFences_) {
        vkDestroyFence(device_, fence, nullptr);
    }
    for (auto sem : imageAvailable_) {
        vkDestroySemaphore(device_, sem, nullptr);
    }
    for (auto sem : renderFinished_) {
        vkDestroySemaphore(device_, sem, nullptr);
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    // 注意：不调用 volkFinalize()——项目存在多个 VulkanDevice（主窗口 + 离屏），
    // 任一 finalize 都会清空全局函数指针，导致后续 device 析构时空指针调用
    // （0xC0000005）。进程退出时由系统回收，泄漏可忽略。
}

bool VulkanDevice::init(WindowHandle window, const RenderConfig& cfg,
                        std::string* outError) {
    window_ = window;
    config_ = cfg;
    validationEnabled_ = cfg.debug;

    // Volk 动态加载（主方案 §6.3 / 附录 依赖：volk）
    if (volkInitialize() != VK_SUCCESS) {
        if (outError) *outError = "volkInitialize failed";
        return false;
    }

    // 实例
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "MolecuLens";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "MolecuLens Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT dbgInfo{};
    if (validationEnabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        dbgInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbgInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbgInfo.pfnUserCallback = debugCallback;
        createInfo.pNext = &dbgInfo;
    }

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateInstance failed";
        return false;
    }
    volkLoadInstance(instance_);

    // 表面
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
        if (outError) *outError = "glfwCreateWindowSurface failed";
        return false;
    }

    if (!pickPhysicalDevice() || !createLogicalDevice() ||
        !createCommandPool() || !createSyncObjects()) {
        if (outError) *outError = "device init failed";
        return false;
    }

    // MSAA 采样数（配置 0/2/4/8 → 最接近支持的）
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts;
    const uint32_t requested = config_.msaaSamples;
    if (requested <= 1) {
        msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    } else {
        VkSampleCountFlagBits best = VK_SAMPLE_COUNT_1_BIT;
        for (uint32_t s : {8u, 4u, 2u}) {
            if (requested >= s && (counts & s)) {
                best = static_cast<VkSampleCountFlagBits>(s);
                break;
            }
            if (s == 2 && !(counts & s)) {
                best = VK_SAMPLE_COUNT_1_BIT;
            }
        }
        msaaSamples_ = best;
    }
    return true;
}

bool VulkanDevice::initHeadless(const RenderConfig& cfg, std::string* outError) {
    window_ = nullptr;
    config_ = cfg;
    validationEnabled_ = cfg.debug;

    if (volkInitialize() != VK_SUCCESS) {
        if (outError) *outError = "volkInitialize failed";
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "MolecuLens CLI";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "MolecuLens Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    if (validationEnabled_) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
    }
    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateInstance failed";
        return false;
    }
    volkLoadInstance(instance_);
    surface_ = VK_NULL_HANDLE;

    // 物理设备：只需 graphics 队列
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int bestScore = 0;
    for (auto device : devices) {
        const int score = rateDevice(device);
        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = device;
        }
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        return false;
    }
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, queues.data());
    for (uint32_t i = 0; i < qCount; ++i) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily_ = i;
            break;
        }
    }
    presentFamily_ = graphicsFamily_;

    if (!createLogicalDevice() || !createCommandPool() || !createSyncObjects()) {
        if (outError) *outError = "headless device init failed";
        return false;
    }
    msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    spdlog::info("[backend] headless Vulkan device ready");
    return true;
}

bool VulkanDevice::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = 0;
    VkPhysicalDevice best = VK_NULL_HANDLE;
    for (auto device : devices) {
        // 队列族检查
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &qCount, queues.data());
        bool hasGraphics = false, hasPresent = false;
        for (uint32_t i = 0; i < qCount; ++i) {
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                hasGraphics = true;
            }
            VkBool32 supportsPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &supportsPresent);
            if (supportsPresent) {
                hasPresent = true;
            }
        }
        if (!hasGraphics || !hasPresent) {
            continue;
        }
        // 扩展检查
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, exts.data());
        bool hasSwapchain = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchain = true;
                break;
            }
        }
        if (!hasSwapchain) {
            continue;
        }
        const int score = rateDevice(device);
        if (score > bestScore) {
            bestScore = score;
            best = device;
        }
    }
    if (best == VK_NULL_HANDLE) {
        return false;
    }
    physicalDevice_ = best;

    // 队列族
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, queues.data());
    for (uint32_t i = 0; i < qCount; ++i) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily_ = i;
            graphicsQueue_ = VK_NULL_HANDLE;  // 延迟
        }
        VkBool32 supportsPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &supportsPresent);
        if (supportsPresent) {
            presentFamily_ = i;
        }
    }
    return true;
}

bool VulkanDevice::createLogicalDevice() {
    std::set<uint32_t> uniqueFamilies = {graphicsFamily_, presentFamily_};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const float priority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queueInfos.push_back(info);
    }

    VkPhysicalDeviceFeatures features{};
    features.fillModeNonSolid = VK_TRUE;   // 线框支持（体系线框模式）
    features.samplerAnisotropy = VK_TRUE;
    features.largePoints = VK_TRUE;        // 点模式：gl_PointSize > 1（体系点模式）

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
    // compute 队列：优先独立 family，否则复用 graphics
    computeQueue_ = graphicsQueue_;
    return true;
}

bool VulkanDevice::createCommandPool() {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = graphicsFamily_;
    if (vkCreateCommandPool(device_, &info, nullptr, &commandPool_) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool VulkanDevice::createSyncObjects() {
    imageAvailable_.resize(kMaxFramesInFlight);
    renderFinished_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &semInfo, nullptr, &imageAvailable_[i]) !=
                VK_SUCCESS ||
            vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinished_[i]) !=
                VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) !=
                VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

// —— 交换链 ——

VkSurfaceFormatKHR VulkanDevice::chooseSurfaceFormat() const {
    // 首选 B8G8R8A8_SRGB + SRGB 色彩空间（主方案 §6.3）
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, fmts.data());
    for (const auto& fmt : fmts) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return fmt;
        }
    }
    return fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB,
                                             VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                        : fmts[0];
}

VkPresentModeKHR VulkanDevice::choosePresentMode() const {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &count,
                                              modes.data());
    // vsync=true → FIFO（真垂直同步，等待 vblank，帧率被显示器刷新率锁定）。
    // 之前优先 MAILBOX：MAILBOX 不等待 vblank、按提交速率尽力呈现 → 帧率不受限、
    // GPU 满负荷空转（FPS 过高）。分子查看器静态画面用 FIFO 最合适。
    if (config_.vsync) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_FIFO_KHR) {
                return m;
            }
        }
    }
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

VkExtent2D VulkanDevice::chooseExtent() const {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    VkExtent2D extent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                               caps.maxImageExtent.height);
    return extent;
}

bool VulkanDevice::createSwapchain() {
    swapchainFormat_ = chooseSurfaceFormat().format;
    swapchainExtent_ = chooseExtent();

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = swapchainFormat_;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = swapchainExtent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (graphicsFamily_ != presentFamily_) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        const uint32_t families[] = {graphicsFamily_, presentFamily_};
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = choosePresentMode();
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;

    if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
        return false;
    }
    if (info.oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, info.oldSwapchain, nullptr);
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchainImages_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data());

    swapchainViews_.clear();
    swapchainViews_.reserve(count);
    for (auto image : swapchainImages_) {
        swapchainViews_.push_back(
            createImageView(image, swapchainFormat_, VK_IMAGE_ASPECT_COLOR_BIT));
    }
    return true;
}

void VulkanDevice::cleanupSwapchain() {
    for (auto view : swapchainViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchainViews_.clear();
    swapchainImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanDevice::destroySwapchain() { cleanupSwapchain(); }

bool VulkanDevice::recreateSwapchain() {
    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
    return createSwapchain();
}

// —— 命令缓冲 ——

VkCommandBuffer VulkanDevice::beginFrameCommands() {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void VulkanDevice::endFrameCommands(VkCommandBuffer cmd) {
    // 注意：这里只结束记录，不释放命令缓冲。释放必须等 GPU 完成使用之后
    //（下一帧同一槽位 beginFrame 的 fence 等待后），否则 vkQueueSubmit 提交
    // 已释放的句柄 = use-after-free（本机驱动表现为黑屏；驱动内解引用 → 0xC0000005）。
    vkEndCommandBuffer(cmd);
}

uint32_t VulkanDevice::acquireNextImage(VkSemaphore signal, uint32_t* outIndex) {
    const VkResult result =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, signal,
                              VK_NULL_HANDLE, outIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return 1;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return 2;
    }
    return 0;
}

VkResult VulkanDevice::presentImage(uint32_t imageIndex, VkSemaphore wait) {
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &imageIndex;
    return vkQueuePresentKHR(presentQueue_, &info);
}

// —— 资源辅助 ——

VkCommandBuffer VulkanDevice::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) {
        spdlog::error("[backend] beginSingleTimeCommands: vkAllocateCommandBuffers failed");
        return VK_NULL_HANDLE;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        spdlog::error("[backend] beginSingleTimeCommands: vkBeginCommandBuffer failed");
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void VulkanDevice::endSingleTimeCommands(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE) {
        return;
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        spdlog::error("[backend] endSingleTimeCommands: vkEndCommandBuffer failed");
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        return;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const VkResult sub = vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE);
    if (sub != VK_SUCCESS) {
        spdlog::error("[backend] endSingleTimeCommands: vkQueueSubmit failed ({})", static_cast<int>(sub));
    }
    vkQueueWaitIdle(graphicsQueue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void VulkanDevice::createImage(VkFormat format, VkExtent2D extent,
                               VkSampleCountFlagBits samples, VkImageTiling tiling,
                               VkImageUsageFlags usage, VkImage* outImage,
                               VkDeviceMemory* outMemory) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = VkExtent3D{extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = tiling;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device_, &info, nullptr, outImage);

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device_, *outImage, &memReq);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = memReq.size;
    uint32_t typeBits = memReq.memoryTypeBits;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if (typeBits & (1u << i) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
        }
    }
    vkAllocateMemory(device_, &alloc, nullptr, outMemory);
    vkBindImageMemory(device_, *outImage, *outMemory, 0);
}

VkImageView VulkanDevice::createImageView(VkImage image, VkFormat format,
                                          VkImageAspectFlags aspect, uint32_t mipLevels) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = mipLevels;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    VkImageView view;
    vkCreateImageView(device_, &info, nullptr, &view);
    return view;
}

} // namespace moleculens::engine
