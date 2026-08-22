// =============================================================================
// MolecuLens · engine · vulkan/desc/descriptors.cpp —— 描述符实现
// =============================================================================

#include "engine/vulkan/desc/descriptors.h"

#include <cstring>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

DescriptorSetLayout::~DescriptorSetLayout() { destroy(); }

bool DescriptorSetLayout::create(VulkanDevice& device,
                                 const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    destroy();
    device_ = &device;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device.device(), &info, nullptr, &layout_) !=
        VK_SUCCESS) {
        spdlog::error("[backend] vkCreateDescriptorSetLayout failed");
        return false;
    }
    return true;
}

void DescriptorSetLayout::destroy() {
    if (layout_ != VK_NULL_HANDLE && device_) {
        vkDestroyDescriptorSetLayout(device_->device(), layout_, nullptr);
    }
    layout_ = VK_NULL_HANDLE;
}

// —— UboDescriptorSet ——

UboDescriptorSet::~UboDescriptorSet() { destroy(); }

bool UboDescriptorSet::create(VulkanDevice& device, VkDescriptorSetLayout layout,
                              VkDeviceSize bufferSize) {
    destroy();
    device_ = &device;
    bufferSize_ = bufferSize;
    const uint32_t kFrames = static_cast<uint32_t>(device.maxFramesInFlight());

    // 池
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kFrames;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFrames;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &pool_) !=
        VK_SUCCESS) {
        return false;
    }

    // sets
    std::vector<VkDescriptorSetLayout> layouts(kFrames, layout);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = kFrames;
    alloc.pSetLayouts = layouts.data();
    sets_.resize(kFrames);
    if (vkAllocateDescriptorSets(device.device(), &alloc, sets_.data()) != VK_SUCCESS) {
        return false;
    }

    // 每帧 UBO 缓冲（HOST_VISIBLE）
    buffers_.resize(kFrames, VK_NULL_HANDLE);
    memories_.resize(kFrames, VK_NULL_HANDLE);
    VkBufferCreateInfo binfo{};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = bufferSize;
    binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (vkCreateBuffer(device.device(), &binfo, nullptr, &buffers_[i]) !=
            VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device.device(), buffers_[i], &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(device.physicalDevice(), &props);
        VkMemoryAllocateInfo ainfo{};
        ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ainfo.allocationSize = req.size;
        for (uint32_t t = 0; t < props.memoryTypeCount; ++t) {
            if ((req.memoryTypeBits & (1u << t)) &&
                (props.memoryTypes[t].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
                ainfo.memoryTypeIndex = t;
                break;
            }
        }
        if (vkAllocateMemory(device.device(), &ainfo, nullptr, &memories_[i]) !=
            VK_SUCCESS) {
            return false;
        }
        vkBindBufferMemory(device.device(), buffers_[i], memories_[i], 0);

        // 绑定 set
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffers_[i];
        bufferInfo.offset = 0;
        bufferInfo.range = bufferSize;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device.device(), 1, &write, 0, nullptr);
    }
    return true;
}

void UboDescriptorSet::update(uint32_t frameIndex, const GlobalUBO& data) {
    if (frameIndex >= buffers_.size() || buffers_[frameIndex] == VK_NULL_HANDLE) {
        return;
    }
    void* mapped = nullptr;
    vkMapMemory(device_->device(), memories_[frameIndex], 0, bufferSize_, 0, &mapped);
    if (mapped) {
        std::memcpy(mapped, &data, sizeof(GlobalUBO));
        vkUnmapMemory(device_->device(), memories_[frameIndex]);
    }
}

VkBuffer UboDescriptorSet::buffer(uint32_t frameIndex) const {
    return frameIndex < buffers_.size() ? buffers_[frameIndex] : VK_NULL_HANDLE;
}

void UboDescriptorSet::destroy() {
    if (pool_ != VK_NULL_HANDLE && device_) {
        vkDestroyDescriptorPool(device_->device(), pool_, nullptr);
    }
    for (auto mem : memories_) {
        if (mem != VK_NULL_HANDLE && device_) {
            vkFreeMemory(device_->device(), mem, nullptr);
        }
    }
    for (auto buf : buffers_) {
        if (buf != VK_NULL_HANDLE && device_) {
            vkDestroyBuffer(device_->device(), buf, nullptr);
        }
    }
    pool_ = VK_NULL_HANDLE;
    sets_.clear();
    buffers_.clear();
    memories_.clear();
}

} // namespace moleculens::engine
