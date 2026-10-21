// =============================================================================
// MolecuLens · engine · vulkan/desc/descriptors.cpp —— 描述符实现
// =============================================================================

#include "engine/vulkan/desc/descriptors.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

void fillGlobalLighting(GlobalUBO& ubo, const LightParams& light) {
    // 半球环境光（既有槽位语义不变）
    ubo.lightDir = glm::vec4(light.skyColor, 0.0f);        // 天顶色
    ubo.lightColor = glm::vec4(light.groundColor, 0.0f);   // 地面色
    ubo.ambient = glm::vec4(light.ambientColor, light.ambientIntensity);

    // 方向光（末尾新增槽）。方向必须归一化：着色器里虽也 normalize，但归一化后
    // 的 w 槽复用为「启用」标志，故此处保证 xyz 单位长、w ∈ {0,1}。
    glm::vec3 dir = light.dirLightDir;
    const float len = glm::length(dir);
    dir = len > 1e-6f ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f);
    const bool on = light.dirLightEnabled && light.dirLightIntensity > 0.0f;
    ubo.dirLightDir = glm::vec4(dir, on ? 1.0f : 0.0f);
    ubo.dirLightColor = glm::vec4(light.dirLightColor, light.dirLightIntensity);
    // lightParams.w = 阴影强度（无方向光或未启用阴影时为 0 → 着色器跳过查表）
    const bool shadowOn = on && light.shadowsEnabled && light.shadowStrength > 0.0f;
    ubo.lightParams = glm::vec4(light.ambientWeight, light.diffuseWeight,
                                light.specularWeight,
                                shadowOn ? light.shadowStrength : 0.0f);
    ubo.lightViewProj = light.lightViewProj;
    ubo.shadowParams = glm::vec4(static_cast<float>(light.shadowMapSize), 0.0f, 0.0f, 0.0f);
}

void fillRenderStyle(GlobalUBO& ubo, const RenderStyle& style,
                     const glm::vec3& backgroundColor) {
    // 夹取到着色器可接受范围：negative wrap 会让明暗交界出现反向亮环；
    // depthCue/outline/rim 超过 1 会把物体压黑或洗白。
    ubo.styleParams = glm::vec4(
        std::clamp(style.wrap, 0.0f, 0.9f),
        std::clamp(style.rim, 0.0f, 1.0f),
        std::clamp(style.outline, 0.0f, 1.0f),
        std::clamp(style.depthCue, 0.0f, 1.0f));
    // 背景是**清屏色的线性值**（_SRGB 附件在写入时做编码）；雾必须淡到同一个值，
    // 否则远处物体会淡向一个与画布不同的颜色，出现"灰边"。
    ubo.backgroundColor = glm::vec4(glm::max(backgroundColor, glm::vec3(0.0f)), 1.0f);
}

void fillClipPlane(GlobalUBO& ubo, const glm::vec4& plane, bool enabled) {
    // 法线长度不做归一化（调用方保证）；w 槽是平面常数 d，不是启用位。
    ubo.clipPlane = plane;
    ubo.clipParams = glm::vec4(enabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
}

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
                              VkDeviceSize bufferSize, VkImageView shadowView,
                              VkSampler shadowSampler) {
    destroy();
    device_ = &device;
    bufferSize_ = bufferSize;
    const bool withShadow =
        shadowView != VK_NULL_HANDLE && shadowSampler != VK_NULL_HANDLE;
    const uint32_t kFrames = static_cast<uint32_t>(device.maxFramesInFlight());

    // 池：UBO 恒需；带阴影时再加 COMBINED_IMAGE_SAMPLER
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFrames;
    if (withShadow) {
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = kFrames;
    }
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFrames;
    poolInfo.poolSizeCount = withShadow ? 2u : 1u;
    poolInfo.pPoolSizes = poolSizes;
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

        // 绑定 set：binding 0 = UBO；binding 1 = 阴影贴图（若提供）
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffers_[i];
        bufferInfo.offset = 0;
        bufferInfo.range = bufferSize;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        VkDescriptorImageInfo imageInfo{};
        if (withShadow) {
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            imageInfo.imageView = shadowView;
            imageInfo.sampler = shadowSampler;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = sets_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &imageInfo;
        }
        vkUpdateDescriptorSets(device.device(), withShadow ? 2u : 1u, writes, 0, nullptr);
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
