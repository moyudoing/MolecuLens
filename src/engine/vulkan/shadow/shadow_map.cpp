// =============================================================================
// MolecuLens · engine · vulkan/shadow/shadow_map.cpp —— 方向光阴影贴图实现
// =============================================================================

#include "engine/vulkan/shadow/shadow_map.h"

#include <cstddef>

#include "spdlog/spdlog.h"

#include "engine/vulkan/device/device.h"

// 内嵌 SPIR-V（顶点阶段复用主 pass 的球/柱着色器，保证几何一致）
#include "gen/shaders/cylinder_vert.h"
#include "gen/shaders/shadow_frag.h"
#include "gen/shaders/sphere_vert.h"

namespace moleculens::engine {

ShadowMap::~ShadowMap() { destroy(); }

bool ShadowMap::create(VulkanDevice& device, uint32_t size,
                       VkDescriptorSetLayout globalLayout, std::string* outError) {
    destroy();
    if (size == 0) {
        if (outError) *outError = "shadow map disabled (size 0)";
        return false;
    }
    device_ = &device;
    size_ = size;
    globalLayout_ = globalLayout;

    // 深度图：既作深度附件，又要被主 pass 采样
    if (!depth_.create(device, VK_FORMAT_D32_SFLOAT, size, size,
                       VK_SAMPLE_COUNT_1_BIT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT)) {
        if (outError) *outError = "shadow depth image create failed";
        return false;
    }
    if (!pass_.createShadowDepth(device, VK_FORMAT_D32_SFLOAT)) {
        if (outError) *outError = "shadow render pass create failed";
        return false;
    }
    if (!fbo_.create(device, pass_.renderPass(), {depth_.view()}, {size, size})) {
        if (outError) *outError = "shadow framebuffer create failed";
        return false;
    }

    // 比较采样器：frag 里用 texture() 手动比较（而非 sampler2DShadow 的硬件比较），
    // 便于做纹素级 PCF 偏移；CLAMP_TO_BORDER + border 深度 1.0 使光照范围外的
    // 采样判定为"未被遮挡"（不产生范围外的假阴影）。
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_FALSE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(device.device(), &si, nullptr, &sampler_) != VK_SUCCESS) {
        if (outError) *outError = "shadow sampler create failed";
        return false;
    }

    if (!lightUbo_.create(device, globalLayout, sizeof(GlobalUBO))) {
        if (outError) *outError = "shadow UBO create failed";
        return false;
    }
    if (!createPipelines(device, outError)) {
        return false;
    }
    initialized_ = true;
    spdlog::info("[backend] shadow map created ({}x{}, D32_SFLOAT)", size, size);
    return true;
}

bool ShadowMap::createPipelines(VulkanDevice& device, std::string* outError) {
    if (!sphereVert_.create(device, VK_SHADER_STAGE_VERTEX_BIT, sphere_vert,
                            sphere_vert_len) ||
        !cylinderVert_.create(device, VK_SHADER_STAGE_VERTEX_BIT, cylinder_vert,
                              cylinder_vert_len) ||
        !shadowFrag_.create(device, VK_SHADER_STAGE_FRAGMENT_BIT, shadow_frag,
                            shadow_frag_len)) {
        if (outError) *outError = "shadow shader module create failed";
        return false;
    }

    // 深度-only 管线：无颜色附件（colorAttachments(0)）、深度测试+写入、无混合。
    // 顶点布局与主 pass 的球/柱管线**逐字段一致**（同一份实例数据 → 同样的几何）。
    // 注意样本数恒为 1x（阴影图不随主 pass 的 MSAA 变化）→ 用 VK_SAMPLE_COUNT_1_BIT。
    const auto build = [&](const ShaderModule& vert,
                           const std::vector<VkVertexInputBindingDescription>& bindings,
                           const std::vector<VkVertexInputAttributeDescription>& attrs,
                           Pipeline* out, const char* name) -> bool {
        const VkPipelineShaderStageCreateInfo stages[] = {vert.stage(), shadowFrag_.stage()};
        PipelineBuilder pb;
        pb.renderPass(pass_.renderPass())
            .samples(VK_SAMPLE_COUNT_1_BIT)
            .topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .shaders(stages, 2)
            .vertexInput(bindings, attrs)
            .descriptorLayouts({globalLayout_})   // 顶点着色器要读 UBO（viewProj）
            .depthTest(true)
            .depthWrite(true)
            .colorAttachments(0)
            .blend(false)
            .cullMode(VK_CULL_MODE_NONE);
        if (!pb.build(device, out)) {
            if (outError) *outError = std::string(name) + " shadow pipeline build failed";
            return false;
        }
        return true;
    };

    {
        const VkVertexInputBindingDescription bindings[] = {
            {0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(SphereInst), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, posRadius)},
            {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, color)},
            {3, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pickId)},
            {4, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pad)},
        };
        if (!build(sphereVert_,
                   std::vector<VkVertexInputBindingDescription>(bindings, bindings + 2),
                   std::vector<VkVertexInputAttributeDescription>(attrs, attrs + 5),
                   &spherePipe_, "sphere")) {
            return false;
        }
    }
    {
        const VkVertexInputBindingDescription bindings[] = {
            {0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX},
            {1, sizeof(CylinderInst), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, aPos)},
            {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, bPos)},
            {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, radius)},
            {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CylinderInst, color)},
            {5, 1, VK_FORMAT_R32G32_UINT, offsetof(CylinderInst, pickId_pad)},
        };
        if (!build(cylinderVert_,
                   std::vector<VkVertexInputBindingDescription>(bindings, bindings + 2),
                   std::vector<VkVertexInputAttributeDescription>(attrs, attrs + 6),
                   &cylinderPipe_, "cylinder")) {
            return false;
        }
    }
    return true;
}

void ShadowMap::destroy() {
    if (device_ == nullptr) {
        return;
    }
    vkDeviceWaitIdle(device_->device());
    spherePipe_.destroy(*device_);
    cylinderPipe_.destroy(*device_);
    sphereVert_.destroy();
    cylinderVert_.destroy();
    shadowFrag_.destroy();
    lightUbo_.destroy();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    fbo_.destroy();
    pass_.destroy();
    depth_.destroy();
    sphereInst_.destroy();
    cylinderInst_.destroy();
    instanceRevision_ = UINT64_MAX;
    sphereCount_ = cylinderCount_ = 0;
    device_ = nullptr;
    initialized_ = false;
}

void ShadowMap::uploadInstances(VulkanDevice& device, const DrawList& draw,
                               uint64_t revision) {
    if (instanceRevision_ == revision) {
        return;   // 绘制列表未变 → 缓冲内容仍有效
    }
    if (!draw.spheres.empty()) {
        const VkDeviceSize need = draw.spheres.size() * sizeof(SphereInst);
        if (!sphereInst_.valid() || sphereInst_.size() < need) {
            sphereInst_.create(device, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        }
        sphereInst_.upload(device, draw.spheres.data(), need);
    }
    if (!draw.cylinders.empty()) {
        const VkDeviceSize need = draw.cylinders.size() * sizeof(CylinderInst);
        if (!cylinderInst_.valid() || cylinderInst_.size() < need) {
            cylinderInst_.create(device, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        }
        cylinderInst_.upload(device, draw.cylinders.data(), need);
    }
    sphereCount_ = static_cast<uint32_t>(draw.spheres.size());
    cylinderCount_ = static_cast<uint32_t>(draw.cylinders.size());
    instanceRevision_ = revision;
}

void ShadowMap::record(VkCommandBuffer cmd, VulkanDevice& device, const DrawList& draw,
                       const SharedMeshes& meshes, const GlobalUBO& lightUbo,
                       uint32_t frameIndex, uint64_t listRevision) {
    if (!initialized_ || spherePipe_.pipeline() == VK_NULL_HANDLE) {
        return;
    }
    uploadInstances(device, draw, listRevision);

    // 无几何时也要走完渲染通道：否则深度图保留上一帧内容（或初始未定义状态），
    // 主 pass 采样会得到错误阴影。
    lightUbo_.update(frameIndex, lightUbo);
    const VkDescriptorSet set = lightUbo_.set(frameIndex);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(size_),
                        static_cast<float>(size_), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {size_, size_}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkClearValue clearDepth = {1.0f, 0};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = pass_.renderPass();
    rp.framebuffer = fbo_.framebuffer();
    rp.renderArea = scissor;
    rp.clearValueCount = 1;
    rp.pClearValues = &clearDepth;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    if (set != VK_NULL_HANDLE) {
        // 球
        if (sphereCount_ > 0 && meshes.icosphereIndices > 0) {
            const Pipeline& p = spherePipe_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0,
                                    1, &set, 0, nullptr);
            VkBuffer vbs[] = {meshes.icosphere.buffer(), sphereInst_.buffer()};
            VkDeviceSize offs[] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(cmd, meshes.icosphereIdx.buffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, meshes.icosphereIndices, sphereCount_, 0, 0, 0);
        }
        // 键圆柱（点模式的键也照常投影 —— 它们仍是遮挡光的实体几何）
        if (cylinderCount_ > 0 && meshes.cylinderIndices > 0) {
            const Pipeline& p = cylinderPipe_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0,
                                    1, &set, 0, nullptr);
            VkBuffer vbs[] = {meshes.cylinder.buffer(), cylinderInst_.buffer()};
            VkDeviceSize offs[] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(cmd, meshes.cylinderIdx.buffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, meshes.cylinderIndices, cylinderCount_, 0, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
}

} // namespace moleculens::engine
