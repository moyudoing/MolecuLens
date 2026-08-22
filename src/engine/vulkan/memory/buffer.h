// =============================================================================
// MolecuLens · engine · vulkan/memory/buffer.h —— VMA 缓冲封装
// 规格来源：主方案 §6.3（缓冲经 VMA 管理；动态数据走 staging → device-local；
//           坐标段用 HOST_VISIBLE 环形缓冲）
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include "engine/vulkan/device/device.h"

namespace moleculens::engine {

class VulkanDevice;

// VMA 缓冲封装（RAII；自动创建/上传/销毁）
class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    bool create(VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage,
                VmaMemoryUsage memUsage);
    // 上传（staging → device-local 或直接 memcpy HOST_VISIBLE）
    void upload(VulkanDevice& device, const void* data, VkDeviceSize bytes,
                VkDeviceSize offset = 0);
    void uploadAsync(VulkanDevice& device, VkCommandBuffer cmd, const void* data,
                     VkDeviceSize bytes, VkDeviceSize offset = 0);
    void* map();
    void unmap();

    VkBuffer buffer() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    bool valid() const { return buffer_ != VK_NULL_HANDLE; }

    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
};

// 图像（MSAA/离屏附件；VMA）
class Image {
public:
    Image() = default;
    ~Image();
    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    bool create(VulkanDevice& device, VkFormat format, uint32_t width, uint32_t height,
                VkSampleCountFlagBits samples, VkImageUsageFlags usage);
    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    // 缓存 allocator：析构时直接使用，避免再次查询静态 allocator 映射
    // （进程退出静态析构阶段，该映射可能已先被销毁 → 0xC0000005）
    VmaAllocator allocator_ = VK_NULL_HANDLE;
};

} // namespace moleculens::engine
