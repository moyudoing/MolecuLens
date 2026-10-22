// =============================================================================
// MolecuLens · engine · vulkan/raytracing/gpu_ray_tracer.cpp —— GPU 光追实现
// 规格来源：任务 E（完整 GPU 光追模块）；主方案 §6.6.4/§6.6.5
//
// 结构：
//   1) 描述符/管线（5 个阶段 + 2 个 hit group + SBT）—— init 时建一次；
//   2) 每帧：图元/顶点/材质 SSBO → BLAS（三角 / AABB 球+胶囊）→ TLAS →
//      vkCmdTraceRaysKHR → 图像回读 RGBA8（行序 top→bottom）；
//   3) 资源生命周期：持久资源析构释放；每帧临时资源在 render 结束统一释放，
//      任何失败路径也会走到（RAII 守卫），不泄漏、不报 validation error。
//
// 输入/输出契约与 CPU engine/raytrace/ray_tracer.h 完全一致；着色公式见
// shaders/rt_common.glsl（与 CPU shade() 同构）。
// =============================================================================

#include "engine/vulkan/raytracing/gpu_ray_tracer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <volk.h>

#include "spdlog/spdlog.h"

#include "engine/raytrace/ray_tracer.h"
#include "engine/vulkan/device/device.h"

// 生成的内嵌 SPIR-V（configure 期由 cmake/shaders.cmake 生成）
#include "gen/shaders/rt_raygen_rgen.h"
#include "gen/shaders/rt_miss_rmiss.h"
#include "gen/shaders/rt_shadow_rmiss.h"
#include "gen/shaders/rt_tri_rchit.h"
#include "gen/shaders/rt_prim_rint.h"

namespace moleculens::engine {
namespace {

// —— 与 shaders/rt_common.glsl 一一对应的 POD（布局必须逐字节一致）——
struct RtPrimRec {
    glm::vec4 geom0;
    glm::vec4 geom1;
    glm::vec4 color;
    glm::vec4 ambient;
    glm::vec4 specular;
    glm::vec4 params;
};
struct RtVertex {
    glm::vec4 pos;
    glm::vec4 nrm;
};
struct RtTriRec {
    glm::uvec4 idx;
};
struct RtMatRec {
    glm::vec4 color;
    glm::vec4 ambient;
    glm::vec4 specular;
    glm::vec4 params;
};
struct RtUniforms {
    glm::vec4 camPos;
    glm::vec4 camForward;
    glm::vec4 camRight;
    glm::vec4 camUp;
    glm::vec4 skyColor;
    glm::vec4 groundColor;
    glm::vec4 ambientColor;
    glm::vec4 dirLightDir;
    glm::vec4 dirLightColor;
    glm::vec4 weights;
    glm::vec4 counts;
    glm::vec4 bgColor;
};
static_assert(sizeof(RtPrimRec) == 96, "RtPrimRec 必须与 GLSL std430 布局一致");
static_assert(sizeof(RtVertex) == 32, "RtVertex 必须与 GLSL 布局一致");
static_assert(sizeof(RtTriRec) == 16, "RtTriRec 必须与 GLSL 布局一致");
static_assert(sizeof(RtMatRec) == 64, "RtMatRec 必须与 GLSL 布局一致");
static_assert(sizeof(RtUniforms) == 12 * 16, "RtUniforms 必须为 12 个 vec4");

constexpr uint32_t kMaxTransparentLayers = 4;   // 任务要求：透明层 ≤4

// SBT 分组（组索引必须与 createPipeline 的 pGroups 顺序一致）
constexpr uint32_t kGroupRaygen = 0;
constexpr uint32_t kGroupMiss = 1;
constexpr uint32_t kGroupShadowMiss = 2;
constexpr uint32_t kGroupTriHit = 3;
constexpr uint32_t kGroupPrimHit = 4;
constexpr uint32_t kGroupCount = 5;
// 命中组在 hit 区内的相对槽位（写入 TLAS instance 的 SBT record offset）
constexpr uint32_t kHitSlotTri = 0;
constexpr uint32_t kHitSlotPrim = 1;

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return a <= 1 ? v : ((v + a - 1) / a) * a;
}

struct Buf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

uint32_t findMemoryType(VkPhysicalDevice physDev, uint32_t typeBits,
                        VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physDev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

// 创建并绑定缓冲；deviceAddress=true 时分配内存带 DEVICE_ADDRESS 标志
bool createBuffer(VulkanDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memProps, bool deviceAddress, Buf* out,
                  bool mapIt = false) {
    out->size = std::max<VkDeviceSize>(size, 4);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = out->size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev.device(), &bi, nullptr, &out->buffer) != VK_SUCCESS) {
        spdlog::error("[rt] vkCreateBuffer failed ({} bytes)", out->size);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device(), out->buffer, &req);
    const uint32_t typeIdx = findMemoryType(dev.physicalDevice(), req.memoryTypeBits,
                                            memProps);
    if (typeIdx == UINT32_MAX) {
        spdlog::error("[rt] no suitable memory type for buffer ({} bytes)",
                      out->size);
        vkDestroyBuffer(dev.device(), out->buffer, nullptr);
        out->buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    if (deviceAddress) {
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIdx;
    if (deviceAddress) {
        ai.pNext = &flagsInfo;
    }
    if (vkAllocateMemory(dev.device(), &ai, nullptr, &out->memory) != VK_SUCCESS) {
        spdlog::error("[rt] vkAllocateMemory failed ({} bytes)", req.size);
        vkDestroyBuffer(dev.device(), out->buffer, nullptr);
        out->buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(dev.device(), out->buffer, out->memory, 0) != VK_SUCCESS) {
        spdlog::error("[rt] vkBindBufferMemory failed");
        vkFreeMemory(dev.device(), out->memory, nullptr);
        vkDestroyBuffer(dev.device(), out->buffer, nullptr);
        out->memory = VK_NULL_HANDLE;
        out->buffer = VK_NULL_HANDLE;
        return false;
    }
    if (mapIt) {
        if (vkMapMemory(dev.device(), out->memory, 0, VK_WHOLE_SIZE, 0,
                        &out->mapped) != VK_SUCCESS) {
            out->mapped = nullptr;
        }
    }
    return true;
}

void destroyBuffer(VulkanDevice& dev, Buf* b) {
    if (b->mapped != nullptr) {
        vkUnmapMemory(dev.device(), b->memory);
        b->mapped = nullptr;
    }
    if (b->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev.device(), b->buffer, nullptr);
    }
    if (b->memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev.device(), b->memory, nullptr);
    }
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->size = 0;
}

uint64_t bufferAddress(VulkanDevice& dev, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(dev.device(), &info);
}

} // namespace

// =============================================================================
// Impl：持久资源 + 每帧临时资源
// =============================================================================
struct GpuRayTracer::Impl {
    VulkanDevice* device = nullptr;
    std::unique_ptr<VulkanDevice> ownedDevice;
    bool initAttempted = false;
    bool rtEnabled = false;
    Stats stats;

    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    Buf sbt;                      // 持久（管线创建后填充，管线不变则复用）
    uint32_t handleSizeAligned = 0;
    uint32_t hitSlotCount = 2;
    // SBT 区域偏移（createPipeline 里算好，render 里按同一组值取地址）
    VkDeviceSize sbtBaseOffset = 0;
    VkDeviceSize sbtRaygenOffset = 0;
    VkDeviceSize sbtMissOffset = 0;
    VkDeviceSize sbtHitOffset = 0;

    // 每帧临时资源（render 结束/失败统一释放）
    std::vector<Buf> transient;
    std::vector<VkAccelerationStructureKHR> transientAs;
    VkImage outImage = VK_NULL_HANDLE;
    VkDeviceMemory outMemory = VK_NULL_HANDLE;
    VkImageView outView = VK_NULL_HANDLE;

    ~Impl() { destroyPersistent(); }

    void destroyTransient() {
        if (device == nullptr) {
            return;
        }
        for (auto as : transientAs) {
            vkDestroyAccelerationStructureKHR(device->device(), as, nullptr);
        }
        transientAs.clear();
        if (outView != VK_NULL_HANDLE) {
            vkDestroyImageView(device->device(), outView, nullptr);
            outView = VK_NULL_HANDLE;
        }
        if (outImage != VK_NULL_HANDLE) {
            vkDestroyImage(device->device(), outImage, nullptr);
            outImage = VK_NULL_HANDLE;
        }
        if (outMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device->device(), outMemory, nullptr);
            outMemory = VK_NULL_HANDLE;
        }
        for (auto& b : transient) {
            destroyBuffer(*device, &b);
        }
        transient.clear();
    }

    void destroyPersistent() {
        if (device == nullptr) {
            return;
        }
        VkDevice d = device->device();
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(d, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        if (pipeLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(d, pipeLayout, nullptr);
            pipeLayout = VK_NULL_HANDLE;
        }
        if (descLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(d, descLayout, nullptr);
            descLayout = VK_NULL_HANDLE;
        }
        if (descPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(d, descPool, nullptr);
            descPool = VK_NULL_HANDLE;
        }
        destroyBuffer(*device, &sbt);
    }

    bool createPipeline(std::string* outError);

    // 在单次命令缓冲里构建一个 BLAS；返回创建的 AS（已登记到 transientAs）。
    bool buildBlas(const VkAccelerationStructureGeometryKHR& geometry,
                   uint32_t primitiveCount, VkAccelerationStructureKHR* outAs,
                   double* elapsedMs);

    bool buildTlas(const VkAccelerationStructureGeometryKHR& geometry,
                   uint32_t instanceCount, VkAccelerationStructureKHR* outAs,
                   double* elapsedMs);

    bool upload(const Buf& dst, const void* data, VkDeviceSize bytes);

    bool createOutputImage(uint32_t w, uint32_t h);
};

// =============================================================================
// 管线（光追阶段 + SBT）
// =============================================================================
bool GpuRayTracer::Impl::createPipeline(std::string* outError) {
    const auto& props = device->rayTracingProperties();
    VulkanDevice& dev = *device;
    const VkDevice vkDev = dev.device();

    // 描述符布局：TLAS / 输出存储图像 / UBO / 4 个 SSBO
    VkDescriptorSetLayoutBinding bindings[7]{};
    const VkShaderStageFlags rtStages =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = rtStages;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = rtStages;
    for (uint32_t i = 3; i < 7; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = rtStages;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(vkDev, &layoutInfo, nullptr, &descLayout) !=
        VK_SUCCESS) {
        if (outError) *outError = "vkCreateDescriptorSetLayout(rt) failed";
        return false;
    }

    VkDescriptorPoolSize poolSizes[4]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = 1;
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[3].descriptorCount = 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(vkDev, &poolInfo, nullptr, &descPool) != VK_SUCCESS) {
        if (outError) *outError = "vkCreateDescriptorPool(rt) failed";
        return false;
    }

    // —— 着色器模块（创建管线后即可销毁）——
    const auto makeModule = [&](const uint8_t* code, size_t bytes, const char* name) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode = reinterpret_cast<const uint32_t*>(code);
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(vkDev, &info, nullptr, &mod) != VK_SUCCESS) {
            spdlog::error("[rt] vkCreateShaderModule failed: {}", name);
        }
        return mod;
    };
    VkShaderModule rgen = makeModule(rt_raygen_rgen, rt_raygen_rgen_len, "rt_raygen");
    VkShaderModule rmiss = makeModule(rt_miss_rmiss, rt_miss_rmiss_len, "rt_miss");
    VkShaderModule smiss =
        makeModule(rt_shadow_rmiss, rt_shadow_rmiss_len, "rt_shadow");
    VkShaderModule rchit = makeModule(rt_tri_rchit, rt_tri_rchit_len, "rt_tri");
    VkShaderModule rint = makeModule(rt_prim_rint, rt_prim_rint_len, "rt_prim");
    if (rgen == VK_NULL_HANDLE || rmiss == VK_NULL_HANDLE ||
        smiss == VK_NULL_HANDLE || rchit == VK_NULL_HANDLE ||
        rint == VK_NULL_HANDLE) {
        for (VkShaderModule m : {rgen, rmiss, smiss, rchit, rint}) {
            if (m != VK_NULL_HANDLE) {
                vkDestroyShaderModule(vkDev, m, nullptr);
            }
        }
        if (outError) *outError = "rt shader module creation failed";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[5]{};
    std::memset(stages, 0, sizeof(stages));
    const VkShaderStageFlagBits stageBits[5] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_INTERSECTION_BIT_KHR};
    VkShaderModule modules[5] = {rgen, rmiss, smiss, rchit, rint};
    for (int i = 0; i < 5; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = stageBits[i];
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }

    VkRayTracingShaderGroupCreateInfoKHR groups[kGroupCount]{};
    for (auto& g : groups) {
        g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        g.generalShader = VK_SHADER_UNUSED_KHR;
        g.closestHitShader = VK_SHADER_UNUSED_KHR;
        g.anyHitShader = VK_SHADER_UNUSED_KHR;
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[kGroupRaygen].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[kGroupRaygen].generalShader = 0;
    groups[kGroupMiss].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[kGroupMiss].generalShader = 1;
    groups[kGroupShadowMiss].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[kGroupShadowMiss].generalShader = 2;
    groups[kGroupTriHit].type =
        VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[kGroupTriHit].closestHitShader = 3;
    groups[kGroupPrimHit].type =
        VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
    groups[kGroupPrimHit].intersectionShader = 4;
    groups[kGroupPrimHit].closestHitShader = 3;

    VkPipelineLayoutCreateInfo pipeLayoutInfo{};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &descLayout;
    if (vkCreatePipelineLayout(vkDev, &pipeLayoutInfo, nullptr, &pipeLayout) !=
        VK_SUCCESS) {
        for (VkShaderModule m : modules) {
            vkDestroyShaderModule(vkDev, m, nullptr);
        }
        if (outError) *outError = "vkCreatePipelineLayout(rt) failed";
        return false;
    }

    VkRayTracingPipelineCreateInfoKHR pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipeInfo.stageCount = 5;
    pipeInfo.pStages = stages;
    pipeInfo.groupCount = kGroupCount;
    pipeInfo.pGroups = groups;
    pipeInfo.maxPipelineRayRecursionDepth =
        std::min(4u, std::max(2u, props.maxRayRecursionDepth));
    pipeInfo.layout = pipeLayout;
    const VkResult pr = vkCreateRayTracingPipelinesKHR(
        vkDev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline);
    for (VkShaderModule m : modules) {
        vkDestroyShaderModule(vkDev, m, nullptr);
    }
    if (pr != VK_SUCCESS) {
        spdlog::error("[rt] vkCreateRayTracingPipelinesKHR failed ({})",
                      static_cast<int>(pr));
        if (outError) *outError = "vkCreateRayTracingPipelinesKHR failed";
        return false;
    }

    // —— SBT：raygen(1) | miss(2) | hit(2)，每区按 shaderGroupBaseAlignment 对齐 ——
    const uint32_t handleSize = props.shaderGroupHandleSize;
    const uint32_t handleAlign = std::max(1u, props.shaderGroupHandleAlignment);
    const uint32_t baseAlign = std::max(1u, props.shaderGroupBaseAlignment);
    handleSizeAligned = static_cast<uint32_t>(alignUp(handleSize, handleAlign));
    const VkDeviceSize raygenOff = 0;
    const VkDeviceSize missOff = alignUp(raygenOff + handleSizeAligned, baseAlign);
    const VkDeviceSize hitOff = alignUp(missOff + 2 * handleSizeAligned, baseAlign);
    const VkDeviceSize tableSize = hitOff + hitSlotCount * handleSizeAligned;

    std::vector<uint8_t> handles(static_cast<size_t>(kGroupCount) * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(vkDev, pipeline, 0, kGroupCount,
                                             handles.size(), handles.data()) !=
        VK_SUCCESS) {
        if (outError) *outError = "vkGetRayTracingShaderGroupHandlesKHR failed";
        return false;
    }

    // 缓冲基址必须对齐到 baseAlignment（raygen 区地址对齐要求）
    if (!createBuffer(dev, tableSize + baseAlign,
                      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      /*deviceAddress=*/true, &sbt, /*mapIt=*/true)) {
        if (outError) *outError = "SBT buffer creation failed";
        return false;
    }
    const VkDeviceSize sbtAddr = bufferAddress(dev, sbt.buffer);
    const VkDeviceSize sbtBaseOff = alignUp(sbtAddr, baseAlign) - sbtAddr;
    sbtBaseOffset = sbtBaseOff;
    sbtRaygenOffset = raygenOff;
    sbtMissOffset = missOff;
    sbtHitOffset = hitOff;
    uint8_t* sbtData = static_cast<uint8_t*>(sbt.mapped);
    if (sbtData == nullptr) {
        if (outError) *outError = "SBT buffer map failed";
        return false;
    }
    const auto putHandle = [&](uint32_t group, VkDeviceSize regionOff) {
        std::memcpy(sbtData + sbtBaseOff + regionOff,
                    handles.data() + static_cast<size_t>(group) * handleSize,
                    handleSize);
    };
    std::memset(sbtData, 0, static_cast<size_t>(sbt.size));
    putHandle(kGroupRaygen, raygenOff);
    putHandle(kGroupMiss, missOff);
    putHandle(kGroupShadowMiss, missOff + handleSizeAligned);
    putHandle(kGroupTriHit, hitOff);
    putHandle(kGroupPrimHit, hitOff + handleSizeAligned);
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = sbt.memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(vkDev, 1, &range);
    return true;
}

// =============================================================================
// 加速结构
// =============================================================================
bool GpuRayTracer::Impl::buildBlas(
    const VkAccelerationStructureGeometryKHR& geometry, uint32_t primitiveCount,
    VkAccelerationStructureKHR* outAs, double* elapsedMs) {
    const auto t0 = std::chrono::steady_clock::now();
    VulkanDevice& dev = *device;
    const VkDevice vkDev = dev.device();

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    uint32_t maxPrim = primitiveCount;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        vkDev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &maxPrim,
        &sizes);

    Buf asBuf{};
    if (!createBuffer(dev, sizes.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, /*deviceAddress=*/true,
                      &asBuf)) {
        return false;
    }
    transient.push_back(asBuf);
    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = asBuf.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    if (vkCreateAccelerationStructureKHR(vkDev, &ci, nullptr, &as) != VK_SUCCESS) {
        spdlog::error("[rt] vkCreateAccelerationStructureKHR(BLAS) failed");
        return false;
    }
    transientAs.push_back(as);

    const uint32_t scratchAlign =
        std::max(1u, device->rayTracingProperties().minScratchOffsetAlignment);
    Buf scratch{};
    if (!createBuffer(dev, sizes.buildScratchSize + scratchAlign,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, /*deviceAddress=*/true,
                      &scratch)) {
        return false;
    }
    transient.push_back(scratch);
    const VkDeviceSize scratchAddr =
        alignUp(bufferAddress(dev, scratch.buffer), scratchAlign);

    build.dstAccelerationStructure = as;
    build.scratchData.deviceAddress = scratchAddr;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    VkCommandBuffer cmd = dev.beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return false;
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &pRange);
    dev.endSingleTimeCommands(cmd);

    *outAs = as;
    if (elapsedMs != nullptr) {
        *elapsedMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    }
    return true;
}

bool GpuRayTracer::Impl::buildTlas(
    const VkAccelerationStructureGeometryKHR& geometry, uint32_t instanceCount,
    VkAccelerationStructureKHR* outAs, double* elapsedMs) {
    const auto t0 = std::chrono::steady_clock::now();
    VulkanDevice& dev = *device;
    const VkDevice vkDev = dev.device();

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    uint32_t maxInstance = instanceCount;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        vkDev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
        &maxInstance, &sizes);

    Buf asBuf{};
    if (!createBuffer(dev, sizes.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, /*deviceAddress=*/true,
                      &asBuf)) {
        return false;
    }
    transient.push_back(asBuf);
    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = asBuf.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    if (vkCreateAccelerationStructureKHR(vkDev, &ci, nullptr, &as) != VK_SUCCESS) {
        spdlog::error("[rt] vkCreateAccelerationStructureKHR(TLAS) failed");
        return false;
    }
    transientAs.push_back(as);

    const uint32_t scratchAlign =
        std::max(1u, device->rayTracingProperties().minScratchOffsetAlignment);
    Buf scratch{};
    if (!createBuffer(dev, sizes.buildScratchSize + scratchAlign,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, /*deviceAddress=*/true,
                      &scratch)) {
        return false;
    }
    transient.push_back(scratch);
    const VkDeviceSize scratchAddr =
        alignUp(bufferAddress(dev, scratch.buffer), scratchAlign);

    build.dstAccelerationStructure = as;
    build.scratchData.deviceAddress = scratchAddr;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    VkCommandBuffer cmd = dev.beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return false;
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &pRange);
    dev.endSingleTimeCommands(cmd);

    *outAs = as;
    if (elapsedMs != nullptr) {
        *elapsedMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    }
    return true;
}

bool GpuRayTracer::Impl::upload(const Buf& dst, const void* data,
                                VkDeviceSize bytes) {
    if (bytes == 0) {
        return true;
    }
    VulkanDevice& dev = *device;
    Buf staging{};
    if (!createBuffer(dev, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      /*deviceAddress=*/false, &staging, /*mapIt=*/true)) {
        return false;
    }
    transient.push_back(staging);
    if (staging.mapped == nullptr) {
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(bytes));
    VkCommandBuffer cmd = dev.beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return false;
    }
    VkBufferCopy region{};
    region.size = bytes;
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    dev.endSingleTimeCommands(cmd);
    return true;
}

bool GpuRayTracer::Impl::createOutputImage(uint32_t w, uint32_t h) {
    VulkanDevice& dev = *device;
    const VkDevice vkDev = dev.device();
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = VkExtent3D{w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vkDev, &info, nullptr, &outImage) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(vkDev, outImage, &req);
    const uint32_t typeIdx = findMemoryType(dev.physicalDevice(), req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (typeIdx == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = typeIdx;
    if (vkAllocateMemory(vkDev, &ai, nullptr, &outMemory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindImageMemory(vkDev, outImage, outMemory, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = outImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(vkDev, &vi, nullptr, &outView) == VK_SUCCESS;
}

// =============================================================================
// 公开 API
// =============================================================================
GpuRayTracer::GpuRayTracer() : impl_(std::make_unique<Impl>()) {}
GpuRayTracer::~GpuRayTracer() = default;

bool GpuRayTracer::available() const {
    return impl_ && impl_->rtEnabled && impl_->device != nullptr;
}

const GpuRayTracer::Stats& GpuRayTracer::lastStats() const {
    static const Stats kEmpty{};
    return impl_ ? impl_->stats : kEmpty;
}

bool GpuRayTracer::init(VulkanDevice* shared, std::string* outError) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    Impl& im = *impl_;
    if (im.rtEnabled) {
        return true;
    }
    if (im.initAttempted) {
        if (outError) *outError = "ray tracing unavailable on this device";
        return false;
    }
    im.initAttempted = true;

    if (shared != nullptr) {
        im.device = shared;
    } else {
        // 没有共享设备（CLI 纯 RT 路径/测试）：自建 headless 设备。
        // MOLECULENS_RT_VALIDATE=1 打开校验层，用于排查光追资源错误。
        auto dev = std::make_unique<VulkanDevice>();
        RenderConfig cfg;
        cfg.debug = std::getenv("MOLECULENS_RT_VALIDATE") != nullptr;
        cfg.msaaSamples = 0;
        cfg.vsync = false;
        std::string err;
        if (!dev->initHeadless(cfg, &err)) {
            if (outError) *outError = "headless Vulkan device init failed: " + err;
            return false;
        }
        im.ownedDevice = std::move(dev);
        im.device = im.ownedDevice.get();
    }

    if (!im.device->rayTracingEnabled()) {
        if (outError) *outError = "device does not support hardware ray tracing";
        if (im.ownedDevice) {
            im.device = nullptr;
            im.ownedDevice.reset();
        }
        return false;
    }
    if (!im.createPipeline(outError)) {
        im.destroyPersistent();
        if (im.ownedDevice) {
            im.device = nullptr;
            im.ownedDevice.reset();
        }
        return false;
    }
    im.rtEnabled = true;
    spdlog::info("[rt] gpu ray tracer ready ({} device)",
                 im.ownedDevice ? "headless" : "shared");
    return true;
}

// =============================================================================
// 渲染：SSBO → BLAS/TLAS → traceRays → 回读
// =============================================================================
bool GpuRayTracer::render(const RtScene& scene, const RtCamera& camera,
                          const RtLight& light, const glm::vec3& backgroundColor,
                          int samplesPerPixel, std::vector<uint8_t>& outRgba) {
    if (!impl_ || !impl_->rtEnabled || impl_->device == nullptr) {
        return false;
    }
    Impl& im = *impl_;
    VulkanDevice& dev = *im.device;
    const VkDevice vkDev = dev.device();
    const uint32_t W = std::max(1u, camera.width);
    const uint32_t H = std::max(1u, camera.height);
    const int spp = std::max(1, samplesPerPixel);
    const size_t pixelCount = static_cast<size_t>(W) * H;
    outRgba.assign(pixelCount * 4, 0);

    // 空场景：与 CPU 完全一致（每像素 miss → 背景色，再做 gamma 编码）
    if (scene.spheres.empty() && scene.cylinders.empty() &&
        scene.triangles.empty()) {
        const auto enc = [](float c) {
            return static_cast<uint8_t>(std::clamp(
                std::pow(std::max(c, 0.0f), 1.0f / 2.2f) * 255.0f, 0.0f, 255.0f));
        };
        const uint8_t r = enc(backgroundColor.r);
        const uint8_t g = enc(backgroundColor.g);
        const uint8_t b = enc(backgroundColor.b);
        for (size_t i = 0; i < pixelCount; ++i) {
            outRgba[i * 4 + 0] = r;
            outRgba[i * 4 + 1] = g;
            outRgba[i * 4 + 2] = b;
            outRgba[i * 4 + 3] = 255;
        }
        im.stats = Stats{};
        return true;
    }

    auto queueLock = dev.lockQueue();   // 与主线程共用 graphics 队列：提交必须串行
    im.destroyTransient();
    struct Cleanup {
        Impl* im;
        VulkanDevice* dev;
        ~Cleanup() {
            im->destroyTransient();
            if (im->descPool != VK_NULL_HANDLE && dev != nullptr) {
                // 队列已 waitIdle（每个单次提交都等了），重置描述符池是安全的
                vkResetDescriptorPool(dev->device(), im->descPool, 0);
            }
        }
    } cleanup{&im, &dev};

    const size_t sphereCount = scene.spheres.size();
    const size_t cylCount = scene.cylinders.size();
    const size_t triCount = scene.triangles.size();

    // —— 1. 图元 SSBO / AABB / 顶点 / 材质 ——
    std::vector<RtPrimRec> prims;
    prims.reserve(sphereCount + cylCount);
    struct Aabb6 {
        float lo[3];
        float hi[3];
    };
    std::vector<Aabb6> aabbs;
    aabbs.reserve(sphereCount + cylCount);
    for (const auto& s : scene.spheres) {
        RtPrimRec p{};
        p.geom0 = glm::vec4(s.center, s.radius);
        p.geom1 = glm::vec4(0.0f);
        p.color = glm::vec4(s.color, 0.0f);
        p.ambient = glm::vec4(s.ambient, 0.0f);
        p.specular = glm::vec4(s.specular, 0.0f);
        p.params = glm::vec4(s.shininess, s.roughness, s.metallic, s.opacity);
        prims.push_back(p);
        Aabb6 box{};
        for (int k = 0; k < 3; ++k) {
            box.lo[k] = s.center[k] - s.radius;
            box.hi[k] = s.center[k] + s.radius;
        }
        aabbs.push_back(box);
    }
    for (const auto& c : scene.cylinders) {
        RtPrimRec p{};
        p.geom0 = glm::vec4(c.a, c.radius);
        p.geom1 = glm::vec4(c.b, 1.0f);
        p.color = glm::vec4(c.color, 0.0f);
        p.ambient = glm::vec4(c.ambient, 0.0f);
        p.specular = glm::vec4(c.specular, 0.0f);
        p.params = glm::vec4(c.shininess, c.roughness, c.metallic, c.opacity);
        prims.push_back(p);
        Aabb6 box{};
        for (int k = 0; k < 3; ++k) {
            box.lo[k] = std::min(c.a[k], c.b[k]) - c.radius;
            box.hi[k] = std::max(c.a[k], c.b[k]) + c.radius;
        }
        aabbs.push_back(box);
    }

    const size_t vertCount = triCount * 3;
    std::vector<RtVertex> verts(vertCount);
    std::vector<RtTriRec> triRecs(triCount);
    std::vector<RtMatRec> triMats(triCount);
    for (size_t i = 0; i < triCount; ++i) {
        const RtTriangle& t = scene.triangles[i];
        verts[i * 3 + 0].pos = glm::vec4(t.v0, 1.0f);
        verts[i * 3 + 0].nrm = glm::vec4(t.n0, 0.0f);
        verts[i * 3 + 1].pos = glm::vec4(t.v1, 1.0f);
        verts[i * 3 + 1].nrm = glm::vec4(t.n1, 0.0f);
        verts[i * 3 + 2].pos = glm::vec4(t.v2, 1.0f);
        verts[i * 3 + 2].nrm = glm::vec4(t.n2, 0.0f);
        triRecs[i].idx =
            glm::uvec4(static_cast<uint32_t>(i * 3),
                       static_cast<uint32_t>(i * 3 + 1),
                       static_cast<uint32_t>(i * 3 + 2), static_cast<uint32_t>(i));
        RtMatRec m{};
        m.color = glm::vec4(t.color, 0.0f);
        m.ambient = glm::vec4(t.ambient, 0.0f);
        m.specular = glm::vec4(t.specular, 0.0f);
        m.params = glm::vec4(t.shininess, t.roughness, t.metallic, t.opacity);
        triMats[i] = m;
    }

    // —— 2. CPU → GPU 缓冲 ——
    RtUniforms u{};
    const glm::vec3 forward = glm::normalize(camera.target - camera.position);
    const glm::vec3 right = glm::normalize(glm::cross(forward, camera.up));
    const glm::vec3 up2 = glm::normalize(glm::cross(right, forward));
    const float tanHalf = std::tan(glm::radians(camera.fovDeg) * 0.5f);
    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const int shadowSamples = std::clamp(light.shadowSamples, 1, 8);
    const int maxBounces = std::clamp(light.maxBounces, 0, 2);
    u.camPos = glm::vec4(camera.position, tanHalf);
    u.camForward = glm::vec4(forward, aspect);
    u.camRight = glm::vec4(right, static_cast<float>(spp));
    u.camUp = glm::vec4(up2, static_cast<float>(W));
    u.skyColor = glm::vec4(light.skyColor, light.ambientIntensity);
    u.groundColor = glm::vec4(light.groundColor, static_cast<float>(shadowSamples));
    u.ambientColor = glm::vec4(light.ambientColor, light.ambientWeight);
    u.dirLightDir = glm::vec4(light.dirLightDir, light.dirLightIntensity);
    u.dirLightColor =
        glm::vec4(light.dirLightColor, light.dirLightEnabled ? 1.0f : 0.0f);
    u.weights = glm::vec4(light.diffuseWeight, light.specularWeight,
                          static_cast<float>(maxBounces), static_cast<float>(H));
    u.counts = glm::vec4(static_cast<float>(sphereCount), static_cast<float>(cylCount),
                         static_cast<float>(triCount),
                         static_cast<float>(kMaxTransparentLayers));
    u.bgColor = glm::vec4(backgroundColor, 0.0f);

    Buf uboBuf{};
    if (!createBuffer(dev, sizeof(RtUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      false, &uboBuf, true)) {
        return false;
    }
    im.transient.push_back(uboBuf);
    if (uboBuf.mapped == nullptr) {
        return false;
    }
    std::memcpy(uboBuf.mapped, &u, sizeof(u));

    const VkBufferUsageFlags ssboUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const VkBufferUsageFlags buildInputUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    Buf primBuf{};
    if (!createBuffer(dev,
                      std::max<size_t>(1, prims.size()) * sizeof(RtPrimRec),
                      ssboUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                      &primBuf)) {
        return false;
    }
    im.transient.push_back(primBuf);
    if (!prims.empty() &&
        !im.upload(primBuf, prims.data(), prims.size() * sizeof(RtPrimRec))) {
        return false;
    }

    Buf vertBuf{};
    if (!createBuffer(dev, std::max<size_t>(1, verts.size()) * sizeof(RtVertex),
                      triCount > 0 ? buildInputUsage : ssboUsage,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, triCount > 0, &vertBuf)) {
        return false;
    }
    im.transient.push_back(vertBuf);
    if (!verts.empty() &&
        !im.upload(vertBuf, verts.data(), verts.size() * sizeof(RtVertex))) {
        return false;
    }

    Buf triBuf{};
    if (!createBuffer(dev, std::max<size_t>(1, triRecs.size()) * sizeof(RtTriRec),
                      ssboUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                      &triBuf)) {
        return false;
    }
    im.transient.push_back(triBuf);
    if (!triRecs.empty() &&
        !im.upload(triBuf, triRecs.data(), triRecs.size() * sizeof(RtTriRec))) {
        return false;
    }

    Buf triMatBuf{};
    if (!createBuffer(dev, std::max<size_t>(1, triMats.size()) * sizeof(RtMatRec),
                      ssboUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false,
                      &triMatBuf)) {
        return false;
    }
    im.transient.push_back(triMatBuf);
    if (!triMats.empty() &&
        !im.upload(triMatBuf, triMats.data(), triMats.size() * sizeof(RtMatRec))) {
        return false;
    }

    const bool hasPrims = !aabbs.empty();
    Buf aabbBuf{};
    if (!createBuffer(dev, std::max<size_t>(1, aabbs.size()) * sizeof(Aabb6),
                      hasPrims ? buildInputUsage : ssboUsage,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hasPrims, &aabbBuf)) {
        return false;
    }
    im.transient.push_back(aabbBuf);
    if (hasPrims &&
        !im.upload(aabbBuf, aabbs.data(), aabbs.size() * sizeof(Aabb6))) {
        return false;
    }

    // —— 3. BLAS（三角 / 球 AABB / 胶囊 AABB）——
    double blasMs = 0.0;
    VkAccelerationStructureKHR triBlas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR sphereBlas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR cylBlas = VK_NULL_HANDLE;

    if (triCount > 0) {
        VkAccelerationStructureGeometryTrianglesDataKHR td{};
        td.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        td.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        td.vertexData.deviceAddress = bufferAddress(dev, vertBuf.buffer);
        td.vertexStride = sizeof(RtVertex);
        td.maxVertex = static_cast<uint32_t>(vertCount - 1);
        td.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = td;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        double ms = 0.0;
        if (!im.buildBlas(geom, static_cast<uint32_t>(triCount), &triBlas, &ms)) {
            return false;
        }
        blasMs += ms;
    }
    if (sphereCount > 0) {
        VkAccelerationStructureGeometryAabbsDataKHR ad{};
        ad.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        ad.data.deviceAddress = bufferAddress(dev, aabbBuf.buffer);
        ad.stride = sizeof(Aabb6);
        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        geom.geometry.aabbs = ad;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        double ms = 0.0;
        if (!im.buildBlas(geom, static_cast<uint32_t>(sphereCount), &sphereBlas,
                          &ms)) {
            return false;
        }
        blasMs += ms;
    }
    if (cylCount > 0) {
        VkAccelerationStructureGeometryAabbsDataKHR ad{};
        ad.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        ad.data.deviceAddress = bufferAddress(dev, aabbBuf.buffer) +
                                sphereCount * sizeof(Aabb6);
        ad.stride = sizeof(Aabb6);
        VkAccelerationStructureGeometryKHR geom{};
        geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        geom.geometry.aabbs = ad;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        double ms = 0.0;
        if (!im.buildBlas(geom, static_cast<uint32_t>(cylCount), &cylBlas, &ms)) {
            return false;
        }
        blasMs += ms;
    }

    // —— 4. TLAS 实例（custom index: 0 球 / 1 胶囊 / 2 三角）——
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(3);
    const auto addInstance = [&](VkAccelerationStructureKHR blas, int custom,
                                 uint32_t hitSlot) {
        if (blas == VK_NULL_HANDLE) {
            return;
        }
        VkAccelerationStructureDeviceAddressInfoKHR dai{};
        dai.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        dai.accelerationStructure = blas;
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = 1.0f;
        inst.transform.matrix[1][1] = 1.0f;
        inst.transform.matrix[2][2] = 1.0f;
        inst.instanceCustomIndex = static_cast<uint32_t>(custom) & 0xFFFFFFu;
        inst.mask = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = hitSlot;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(vkDev, &dai);
        instances.push_back(inst);
    };
    addInstance(triBlas, 2, kHitSlotTri);
    addInstance(sphereBlas, 0, kHitSlotPrim);
    addInstance(cylBlas, 1, kHitSlotPrim);

    Buf instBuf{};
    if (!createBuffer(dev,
                      std::max<size_t>(1, instances.size()) *
                          sizeof(VkAccelerationStructureInstanceKHR),
                      buildInputUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true,
                      &instBuf)) {
        return false;
    }
    im.transient.push_back(instBuf);
    if (!instances.empty() &&
        !im.upload(instBuf, instances.data(),
                   instances.size() * sizeof(VkAccelerationStructureInstanceKHR))) {
        return false;
    }

    VkAccelerationStructureGeometryInstancesDataKHR id{};
    id.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    id.arrayOfPointers = VK_FALSE;
    id.data.deviceAddress = bufferAddress(dev, instBuf.buffer);
    VkAccelerationStructureGeometryKHR tlasGeom{};
    tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.geometry.instances = id;
    tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    double tlasMs = 0.0;
    if (!im.buildTlas(tlasGeom, static_cast<uint32_t>(instances.size()), &tlas,
                      &tlasMs)) {
        return false;
    }

    // —— 5. 输出图像 + 回读缓冲 ——
    if (!im.createOutputImage(W, H)) {
        return false;
    }
    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(pixelCount) * 4;
    Buf readback{};
    if (!createBuffer(dev, pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      false, &readback, true)) {
        return false;
    }
    im.transient.push_back(readback);
    if (readback.mapped == nullptr) {
        return false;
    }

    // —— 6. 描述符 ——
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = im.descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &im.descLayout;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkDev, &dsai, &dset) != VK_SUCCESS) {
        spdlog::error("[rt] vkAllocateDescriptorSets failed");
        return false;
    }
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas;
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = im.outView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo dbufs[5]{};
    dbufs[0].buffer = uboBuf.buffer;
    dbufs[0].offset = 0;
    dbufs[0].range = sizeof(RtUniforms);
    dbufs[1].buffer = primBuf.buffer;
    dbufs[1].offset = 0;
    dbufs[1].range = VK_WHOLE_SIZE;
    dbufs[2].buffer = vertBuf.buffer;
    dbufs[2].offset = 0;
    dbufs[2].range = VK_WHOLE_SIZE;
    dbufs[3].buffer = triBuf.buffer;
    dbufs[3].offset = 0;
    dbufs[3].range = VK_WHOLE_SIZE;
    dbufs[4].buffer = triMatBuf.buffer;
    dbufs[4].offset = 0;
    dbufs[4].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[7]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = dset;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = dset;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &imageInfo;
    for (uint32_t i = 0; i < 5; ++i) {
        writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2 + i].dstSet = dset;
        writes[2 + i].dstBinding = 2 + i;
        writes[2 + i].descriptorCount = 1;
        writes[2 + i].descriptorType = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2 + i].pBufferInfo = &dbufs[i];
    }
    vkUpdateDescriptorSets(vkDev, 7, writes, 0, nullptr);

    // —— 7. traceRays + 回读 ——
    const VkDeviceSize sbtAddr = bufferAddress(dev, im.sbt.buffer);
    const VkDeviceSize stride = im.handleSizeAligned;
    VkStridedDeviceAddressRegionKHR raygenRegion{};
    raygenRegion.deviceAddress =
        sbtAddr + im.sbtBaseOffset + im.sbtRaygenOffset;
    raygenRegion.stride = stride;
    raygenRegion.size = stride;
    VkStridedDeviceAddressRegionKHR missRegion{};
    missRegion.deviceAddress = sbtAddr + im.sbtBaseOffset + im.sbtMissOffset;
    missRegion.stride = stride;
    missRegion.size = stride * 2;
    VkStridedDeviceAddressRegionKHR hitRegion{};
    hitRegion.deviceAddress = sbtAddr + im.sbtBaseOffset + im.sbtHitOffset;
    hitRegion.stride = stride;
    hitRegion.size = stride * im.hitSlotCount;
    VkStridedDeviceAddressRegionKHR callableRegion{};

    const auto traceStart = std::chrono::steady_clock::now();
    VkCommandBuffer cmd = dev.beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
        return false;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = im.outImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, im.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            im.pipeLayout, 0, 1, &dset, 0, nullptr);
    vkCmdTraceRaysKHR(cmd, &raygenRegion, &missRegion, &hitRegion, &callableRegion,
                      W, H, 1);

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = VkExtent3D{W, H, 1};
    vkCmdCopyImageToBuffer(cmd, im.outImage,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                           1, &copy);
    VkMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, nullptr,
                         0, nullptr);
    dev.endSingleTimeCommands(cmd);   // 提交并 vkQueueWaitIdle

    std::memcpy(outRgba.data(), readback.mapped, static_cast<size_t>(pixelBytes));
    const double traceMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - traceStart)
            .count();

    im.stats.triangles = triCount;
    im.stats.primitives = sphereCount + cylCount;
    im.stats.blasMs = blasMs;
    im.stats.tlasMs = tlasMs;
    im.stats.traceMs = traceMs;
    spdlog::info(
        "[rt] gpu: {} tris / {} prims, BLAS {:.1f} ms, TLAS {:.1f} ms, "
        "trace {:.1f} ms",
        im.stats.triangles, im.stats.primitives, im.stats.blasMs, im.stats.tlasMs,
        im.stats.traceMs);
    return true;
}

} // namespace moleculens::engine
