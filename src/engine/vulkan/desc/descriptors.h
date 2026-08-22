// =============================================================================
// MolecuLens · engine · vulkan/desc/descriptors.h —— 描述符
// 规格来源：主方案 §6.3（全局 UBO：相机 + 光照 + 场景属性，set 0 binding 0）
// =============================================================================

#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <volk.h>

#include "engine/vulkan/device/device.h"

namespace moleculens::engine {

// 全局 UBO 布局（与 shaders/global_ubo.glsl 对应，160B）
struct GlobalUBO {
    glm::mat4 viewProj;        // 0-64
    glm::vec4 cameraPos;       // 64
    glm::vec4 lightDir;        // 80（指向光源方向）
    glm::vec4 lightColor;      // 96
    glm::vec4 ambient;         // 112（rgb 色 + w 强度）
    glm::vec4 fog;             // 128（x near, y far, z mode, w density）
    glm::vec4 sceneProps;      // 144（x 全局缩放，yzw 保留）
};
static_assert(sizeof(GlobalUBO) == 160);

class DescriptorSetLayout {
public:
    DescriptorSetLayout() = default;
    ~DescriptorSetLayout();
    bool create(VulkanDevice& device, const std::vector<VkDescriptorSetLayoutBinding>&);
    VkDescriptorSetLayout layout() const { return layout_; }
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
};

// UBO 描述符池 + 每帧 set（环形 kMaxFramesInFlight）
class UboDescriptorSet {
public:
    UboDescriptorSet() = default;
    ~UboDescriptorSet();
    bool create(VulkanDevice& device, VkDescriptorSetLayout layout,
                VkDeviceSize bufferSize);
    void update(uint32_t frameIndex, const GlobalUBO& data);
    VkDescriptorSet set(uint32_t frameIndex) const {
        return frameIndex < sets_.size() ? sets_[frameIndex] : VK_NULL_HANDLE;
    }
    VkBuffer buffer(uint32_t frameIndex) const;
    void destroy();

private:
    VulkanDevice* device_ = nullptr;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> sets_;
    std::vector<VkBuffer> buffers_;
    std::vector<VkDeviceMemory> memories_;
    VkDeviceSize bufferSize_ = 0;
};

} // namespace moleculens::engine
