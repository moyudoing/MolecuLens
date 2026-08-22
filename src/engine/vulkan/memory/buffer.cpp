// =============================================================================
// MolecuLens · engine · vulkan/memory/buffer.cpp —— VMA 缓冲实现
// 规格来源：主方案 §6.3
// =============================================================================

#include "engine/vulkan/memory/buffer.h"

#include <cstring>
#include <unordered_map>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

namespace {

VmaAllocator allocatorFor(VulkanDevice& device) {
    // 每 device 一个 VMA allocator（主窗口与离屏 headless 是两个 device，
    // 全局单例会跨设备分配资源 → vulkan-1.dll 崩溃）
    // 映射用堆分配且永不释放：进程退出静态析构阶段，若此映射先于仍持有
    // Buffer/Image 的对象被销毁，后续析构里 allocators.find 会访问已释放内存
    // → 0xC0000005（静态析构顺序未定义）。
    static std::unordered_map<VkDevice, VmaAllocator>* allocators =
        new std::unordered_map<VkDevice, VmaAllocator>();
    const VkDevice dev = device.device();
    auto it = allocators->find(dev);
    if (it != allocators->end()) {
        return it->second;
    }
    VmaAllocatorCreateInfo info{};
    info.device = dev;
    info.physicalDevice = device.physicalDevice();
    info.instance = device.instance();
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    // VMA 3.x 动态函数（VMA_DYNAMIC_VULKAN_FUNCTIONS）要求显式传递 ProcAddr
    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    info.pVulkanFunctions = &vf;
    VmaAllocator allocator = VK_NULL_HANDLE;
    if (vmaCreateAllocator(&info, &allocator) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    allocators->emplace(dev, allocator);
    return allocator;
}

} // namespace

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer&& other) noexcept
    : device_(other.device_), buffer_(other.buffer_),
      allocation_(other.allocation_), size_(other.size_),
      allocator_(other.allocator_) {
    other.buffer_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.allocator_ = VK_NULL_HANDLE;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        size_ = other.size_;
        allocator_ = other.allocator_;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.allocator_ = VK_NULL_HANDLE;
    }
    return *this;
}

bool Buffer::create(VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                    VmaMemoryUsage memUsage) {
    destroy();
    device_ = &device;
    allocator_ = allocatorFor(device);
    if (allocator_ == VK_NULL_HANDLE) {
        return false;
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memUsage;
    if (memUsage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST ||
        memUsage == VMA_MEMORY_USAGE_AUTO) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    if (vmaCreateBuffer(allocator_, &info, &allocInfo, &buffer_, &allocation_,
                        nullptr) != VK_SUCCESS) {
        spdlog::error("[backend] vmaCreateBuffer failed ({} bytes)", size);
        return false;
    }
    size_ = size;
    return true;
}

void Buffer::upload(VulkanDevice& device, const void* data, VkDeviceSize bytes,
                    VkDeviceSize offset) {
    if (!valid() || bytes == 0) {
        return;
    }
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(allocator_, allocation_, &allocInfo);
    if (allocInfo.pMappedData != nullptr) {
        std::memcpy(static_cast<uint8_t*>(allocInfo.pMappedData) + offset, data, bytes);
        vmaFlushAllocation(allocator_, allocation_, offset, bytes);
        return;
    }
    // staging 路径
    Buffer staging;
    staging.create(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    VmaAllocationInfo stagingInfo;
    vmaGetAllocationInfo(staging.allocator_, staging.allocation_, &stagingInfo);
    if (stagingInfo.pMappedData) {
        std::memcpy(stagingInfo.pMappedData, data, bytes);
        vmaFlushAllocation(staging.allocator_, staging.allocation_, 0, bytes);
    }
    VkCommandBuffer cmd = device.beginSingleTimeCommands();
    VkBufferCopy region{0, offset, bytes};
    vkCmdCopyBuffer(cmd, staging.buffer_, buffer_, 1, &region);
    device.endSingleTimeCommands(cmd);
}

void Buffer::uploadAsync(VulkanDevice& device, VkCommandBuffer cmd, const void* data,
                         VkDeviceSize bytes, VkDeviceSize offset) {
    if (!valid() || bytes == 0) {
        return;
    }
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(allocator_, allocation_, &allocInfo);
    if (allocInfo.pMappedData != nullptr) {
        std::memcpy(static_cast<uint8_t*>(allocInfo.pMappedData) + offset, data, bytes);
        vmaFlushAllocation(allocator_, allocation_, offset, bytes);
        return;
    }
    // staging：本函数只支持 HOST_VISIBLE 快速路径（帧循环内使用）
    (void)device;
    (void)cmd;
    spdlog::error("[backend] uploadAsync requires host-visible buffer");
}

void* Buffer::map() {
    if (!valid()) {
        return nullptr;
    }
    void* data = nullptr;
    if (vmaMapMemory(allocator_, allocation_, &data) != VK_SUCCESS) {
        return nullptr;
    }
    return data;
}

void Buffer::unmap() {
    if (valid()) {
        vmaUnmapMemory(allocator_, allocation_);
    }
}

void Buffer::destroy() {
    if (buffer_ != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    size_ = 0;
}

// —— Image ——

Image::~Image() { destroy(); }

Image::Image(Image&& other) noexcept
    : device_(other.device_), image_(other.image_), view_(other.view_),
      allocation_(other.allocation_), allocator_(other.allocator_) {
    other.image_ = VK_NULL_HANDLE;
    other.view_ = VK_NULL_HANDLE;
    other.allocation_ = VK_NULL_HANDLE;
    other.allocator_ = VK_NULL_HANDLE;
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = other.device_;
        image_ = other.image_;
        view_ = other.view_;
        allocation_ = other.allocation_;
        allocator_ = other.allocator_;
        other.image_ = VK_NULL_HANDLE;
        other.view_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.allocator_ = VK_NULL_HANDLE;
    }
    return *this;
}

bool Image::create(VulkanDevice& device, VkFormat format, uint32_t width,
                   uint32_t height, VkSampleCountFlagBits samples,
                   VkImageUsageFlags usage) {
    destroy();
    device_ = &device;
    allocator_ = allocatorFor(device);
    if (allocator_ == VK_NULL_HANDLE) {
        return false;
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = VkExtent3D{width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocator_, &info, &allocInfo, &image_, &allocation_,
                       nullptr) != VK_SUCCESS) {
        return false;
    }
    const VkImageAspectFlags aspect =
        (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_ASPECT_COLOR_BIT
                                                      : VK_IMAGE_ASPECT_DEPTH_BIT;
    view_ = device.createImageView(image_, format, aspect);
    return true;
}

void Image::destroy() {
    if (view_ != VK_NULL_HANDLE && device_) {
        vkDestroyImageView(device_->device(), view_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        // 直接用缓存的 allocator，避免在进程退出静态析构阶段再次查询
        // 静态 allocator 映射（可能已先被销毁 → 0xC0000005）
        vmaDestroyImage(allocator_, image_, allocation_);
    }
    image_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
}

} // namespace moleculens::engine
