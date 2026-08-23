// =============================================================================
// MolecuLens · engine · vulkan/offscreen/offscreen_renderer.cpp —— 离屏渲染实现
// 规格来源：主方案 §6.1/§12.5（无 Swapchain；MSAA resolve；像素回读 RGBA8/16F）
// =============================================================================

#include "engine/vulkan/offscreen/offscreen_renderer.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <unordered_map>

#include "spdlog/spdlog.h"

#include "engine/render/renderer.h"

namespace moleculens::engine {

namespace {

#include "gen/shaders/sphere_vert.h"
#include "gen/shaders/sphere_frag.h"
#include "gen/shaders/cylinder_vert.h"
#include "gen/shaders/cylinder_frag.h"
#include "gen/shaders/line_vert.h"
#include "gen/shaders/line_frag.h"
#include "gen/shaders/mesh_vert.h"
#include "gen/shaders/mesh_frag.h"
#include "gen/shaders/point_vert.h"
#include "gen/shaders/point_frag.h"

glm::mat4 vkProjection(const glm::mat4& proj) {
    glm::mat4 m = proj;
    m[1][1] *= -1.0f;
    // Vulkan 深度范围 [0,1]（GLM 默认 [-1,1]）：clip.z' = 0.5*clip.z + 0.5*clip.w。
    // GLM 透视矩阵（列主序 m[col][row]）：z 行 = (m[0][2], m[1][2], m[2][2], m[3][2])，
    //   m[2][2] = z 系数(A)，m[3][2] = w 系数(B)；w 行 = (m[0][3]..m[3][3])，m[2][3] = -1。
    // 故 z 系数' = 0.5*A + 0.5*(-1)（读 m[2][3]），w 系数' = 0.5*B（写 m[3][2]），
    // clip.w 保持不变（-z）。
    // 此前误把 m[3][2] 当 z 系数读取、把 m[2][3] 当 w 系数写入 → clip.w 变 -0.5z、
    // 深度映射错误 → 分子位于远平面外被裁剪 → 视口空白（见 moleculens-fixes）。
    m[2][2] = m[2][2] * 0.5f + m[2][3] * 0.5f;
    m[3][2] = m[3][2] * 0.5f;
    return m;
}

} // namespace

namespace {
// 网格生成（与 realtime 共享实现；放匿名命名空间避免重复符号）
// 球体细分：2 级 → 320 三角形（比原 80 更平滑；主方案 §5.7.1）
void subdivideOnce(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    std::vector<glm::vec3> newVerts = verts;
    std::vector<uint32_t> newIndices;
    newIndices.reserve(indices.size() * 4);
    std::unordered_map<uint64_t, uint32_t> midCache;
    auto mid = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint32_t lo = std::min(a, b), hi = std::max(a, b);
        const uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
        auto it = midCache.find(key);
        if (it != midCache.end()) {
            return it->second;
        }
        const glm::vec3 m = glm::normalize(verts[lo] + verts[hi]);
        const uint32_t idx = static_cast<uint32_t>(newVerts.size());
        newVerts.push_back(m);
        midCache.emplace(key, idx);
        return idx;
    };
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const uint32_t m01 = mid(i0, i1), m12 = mid(i1, i2), m20 = mid(i2, i0);
        newIndices.insert(newIndices.end(),
                          {i0, m01, m20, m01, i1, m12, m20, m12, i2, m01, m12, m20});
    }
    verts.swap(newVerts);
    indices.swap(newIndices);
}

void generateIcosphereImpl(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const glm::vec3 base[] = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    for (const auto& v : base) {
        verts.push_back(glm::normalize(v));
    }
    const uint32_t faces[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    for (const auto& f : faces) {
        const glm::vec3 v0 = verts[f[0]], v1 = verts[f[1]], v2 = verts[f[2]];
        const glm::vec3 m01 = glm::normalize(v0 + v1);
        const glm::vec3 m12 = glm::normalize(v1 + v2);
        const glm::vec3 m20 = glm::normalize(v2 + v0);
        const uint32_t j0 = static_cast<uint32_t>(verts.size());
        const uint32_t j1 = j0 + 1, j2 = j0 + 2;
        verts.push_back(m01);
        verts.push_back(m12);
        verts.push_back(m20);
        indices.insert(indices.end(), {f[0], j0, j2, j0, f[1], j1, j2, j1, f[2], j0, j1, j2});
    }
    subdivideOnce(verts, indices);   // 2 级：80 → 320 三角形
}

void generateCylinderImpl(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    constexpr int kSegments = 8;
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (int i = 0; i < kSegments; ++i) {
        const float a = 6.2831853f * i / kSegments;
        const float ca = std::cos(a), sa = std::sin(a);
        verts.push_back(glm::vec3(ca, -1.0f, sa));
        verts.push_back(glm::vec3(ca, 1.0f, sa));
    }
    for (int i = 0; i < kSegments; ++i) {
        const uint32_t i0 = base + static_cast<uint32_t>(i) * 2;
        const uint32_t i1 = i0 + 1;
        const uint32_t i2 = base + static_cast<uint32_t>((i + 1) % kSegments) * 2;
        const uint32_t i3 = i2 + 1;
        indices.insert(indices.end(), {i0, i1, i2, i1, i3, i2});
    }
}
} // namespace

// —— 生命周期 ——

OffscreenRenderer::~OffscreenRenderer() { shutdown(); }

bool OffscreenRenderer::init(const RenderConfig& cfg, uint32_t width, uint32_t height,
                             std::string* outError) {
    shutdown();
    config_ = cfg;
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);

    ownedDevice_ = std::make_unique<VulkanDevice>();
    device_ = ownedDevice_.get();
    if (!device_->initHeadless(config_, outError)) {
        ownedDevice_.reset();
        device_ = nullptr;
        return false;
    }
    if (!initCommon(outError)) {
        ownedDevice_.reset();
        device_ = nullptr;
        return false;
    }
    return true;
}

// 共享主设备初始化（GUI 视口路径）：FBO/管线建在主设备上，ImGui 描述符集
// 与图像同设备采样；headless 路径（CLI 导出）仍走 init()。
bool OffscreenRenderer::initShared(VulkanDevice& device, uint32_t width,
                                   uint32_t height, std::string* outError) {
    shutdown();
    config_ = device.config();
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    device_ = &device;
    if (!initCommon(outError)) {
        device_ = nullptr;
        return false;
    }
    return true;
}

bool OffscreenRenderer::initCommon(std::string* outError) {
    if (!createFramebuffer()) {
        if (outError) *outError = "offscreen framebuffer failed";
        return false;
    }

    // 管线（与 realtime 相同布局；全局光照 + 材质 push constant）
    // 每体系渲染模式一套管线（实心/线框/透明/点，见 SystemRenderMode）：
    // 实心=填充三角；线框=polygon LINE（fillModeNonSolid 特性）；透明=混合+关深度写；
    // 点=GL_POINTS 点管线（SphereInst 实例数据 → 点）。
    auto loadShader = [&](ShaderModule*& out, VkShaderStageFlagBits stage,
                          const uint8_t* spv, size_t size) -> bool {
        out = new ShaderModule();
        return out->create(*device_, stage, spv, size);
    };
    ShaderModule* sphereVert = nullptr;
    ShaderModule* sphereFrag = nullptr;
    ShaderModule* cylinderVert = nullptr;
    ShaderModule* cylinderFrag = nullptr;
    ShaderModule* lineVert = nullptr;
    ShaderModule* lineFrag = nullptr;
    ShaderModule* meshVert = nullptr;
    ShaderModule* meshFrag = nullptr;
    ShaderModule* pointVert = nullptr;
    ShaderModule* pointFrag = nullptr;
    if (!loadShader(sphereVert, VK_SHADER_STAGE_VERTEX_BIT, sphere_vert, sphere_vert_len) ||
        !loadShader(sphereFrag, VK_SHADER_STAGE_FRAGMENT_BIT, sphere_frag, sphere_frag_len) ||
        !loadShader(cylinderVert, VK_SHADER_STAGE_VERTEX_BIT, cylinder_vert, cylinder_vert_len) ||
        !loadShader(cylinderFrag, VK_SHADER_STAGE_FRAGMENT_BIT, cylinder_frag, cylinder_frag_len) ||
        !loadShader(lineVert, VK_SHADER_STAGE_VERTEX_BIT, line_vert, line_vert_len) ||
        !loadShader(lineFrag, VK_SHADER_STAGE_FRAGMENT_BIT, line_frag, line_frag_len) ||
        !loadShader(meshVert, VK_SHADER_STAGE_VERTEX_BIT, mesh_vert, mesh_vert_len) ||
        !loadShader(meshFrag, VK_SHADER_STAGE_FRAGMENT_BIT, mesh_frag, mesh_frag_len) ||
        !loadShader(pointVert, VK_SHADER_STAGE_VERTEX_BIT, point_vert, point_vert_len) ||
        !loadShader(pointFrag, VK_SHADER_STAGE_FRAGMENT_BIT, point_frag, point_frag_len)) {
        delete sphereVert; delete sphereFrag; delete cylinderVert; delete cylinderFrag;
        delete lineVert; delete lineFrag; delete meshVert; delete meshFrag;
        delete pointVert; delete pointFrag;
        return false;
    }
    const auto freeShaders = [&]() {
        delete sphereVert; delete sphereFrag; delete cylinderVert; delete cylinderFrag;
        delete lineVert; delete lineFrag; delete meshVert; delete meshFrag;
        delete pointVert; delete pointFrag;
    };

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!globalLayout_.create(*device_, {binding}) ||
        !globalUbo_.create(*device_, globalLayout_.layout(), sizeof(GlobalUBO))) {
        freeShaders();
        return false;
    }

    const VkRenderPass rp = renderPass_.renderPass();
    // 离屏 FBO 附件恒为 1x（createFramebuffer），管线采样数必须与之一致。
    // 共享主设备（MSAA 4x）时不可用 device.msaaSamples()——采样数不匹配的
    // 管线与渲染通道不兼容 → 全屏片元不绘制 → 分子空白。
    const VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    // 管线构建辅助（不同模式 = 不同 VkPipeline，见 SystemRenderMode）
    struct PipeSpec {
        const VkPipelineShaderStageCreateInfo* stages = nullptr;   // 2 个（vert/frag）
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attrs;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        bool blend = false;
        bool depthWrite = true;
        VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
        const char* name = "pipeline";
    };
    auto buildPipe = [&](const PipeSpec& s, Pipeline* out) -> bool {
        PipelineBuilder pb;
        pb.renderPass(rp).samples(samples).topology(s.topology)
            .shaders(s.stages, 2)
            .vertexInput(s.bindings, s.attrs)
            .descriptorLayouts({globalLayout_.layout()})
            .pushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MaterialBlock))
            .depthTest(true).depthWrite(s.depthWrite).blend(s.blend)
            .polygonMode(s.polygon).cullMode(VK_CULL_MODE_NONE);
        if (!pb.build(*device_, out)) {
            if (outError) *outError = std::string(s.name) + " pipeline build failed";
            return false;
        }
        return true;
    };

    // —— 球（SphereInst 实例化）：实心 / 线框 / 透明 ——
    {
        const VkPipelineShaderStageCreateInfo sphereStages[] = {sphereVert->stage(), sphereFrag->stage()};
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
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 2);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 5);
        if (!buildPipe({sphereStages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "sphere"}, &spherePipeline_) ||
            !buildPipe({sphereStages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE, "sphere-wire"}, &sphereWire_) ||
            !buildPipe({sphereStages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "sphere-blend"}, &sphereBlend_)) {
            freeShaders();
            return false;
        }
    }
    // —— 键圆柱（CylinderInst）：实心 / 线框 / 透明 ——
    {
        const VkPipelineShaderStageCreateInfo cylStages[] = {cylinderVert->stage(), cylinderFrag->stage()};
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
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 2);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 6);
        if (!buildPipe({cylStages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "cylinder"}, &cylinderPipeline_) ||
            !buildPipe({cylStages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE, "cylinder-wire"}, &cylinderWire_) ||
            !buildPipe({cylStages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "cylinder-blend"}, &cylinderBlend_)) {
            freeShaders();
            return false;
        }
    }
    // —— 线段（LineInst；点模式的键也走此管线）——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {lineVert->stage(), lineFrag->stage()};
        // 顶点布局 = pos(float4) + color(float4) 交错 = 32B/顶点（line.vert）；
        // 原 48B stride 与数据不匹配 → 顶点取数错位 → 线框渲染错乱。
        const VkVertexInputBindingDescription bindings[] = {
            {0, 32, VK_VERTEX_INPUT_RATE_VERTEX},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 1);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 2);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "line"}, &linePipeline_)) {
            freeShaders();
            return false;
        }
    }
    // —— 三角网格（TriMeshVertex 48B）：实心 / 线框 / 透明 ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {meshVert->stage(), meshFrag->stage()};
        const VkVertexInputBindingDescription bindings[] = {
            {0, sizeof(TriMeshVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TriMeshVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TriMeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TriMeshVertex, color)},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 1);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 3);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "mesh"}, &meshPipeline_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE, "mesh-wire"}, &meshWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL, "mesh-blend"}, &meshBlend_)) {
            freeShaders();
            return false;
        }
    }
    // —— 点（GL_POINTS；SphereInst 实例数据 → 原子点）——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {pointVert->stage(), pointFrag->stage()};
        // 仅实例绑定（binding 1，SphereInst）；无顶点绑定（point.vert 无 location 0）
        const VkVertexInputBindingDescription bindings[] = {
            {1, sizeof(SphereInst), VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, posRadius)},
            {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SphereInst, color)},
            {3, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pickId)},
            {4, 1, VK_FORMAT_R32G32_UINT, offsetof(SphereInst, pad)},
        };
        const std::vector<VkVertexInputBindingDescription> b(bindings, bindings + 1);
        const std::vector<VkVertexInputAttributeDescription> a(attrs, attrs + 4);
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, false, true,
                        VK_POLYGON_MODE_FILL, "point"}, &point_)) {
            freeShaders();
            return false;
        }
    }
    freeShaders();

    // 共享网格
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> indices;
    generateIcosphereImpl(verts, indices);
    icosphereVerts_ = static_cast<uint32_t>(verts.size());
    icosphereIndices_ = static_cast<uint32_t>(indices.size());
    icosphere_ = std::make_unique<Buffer>();
    icosphere_->create(*device_, verts.size() * sizeof(glm::vec3),
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    icosphere_->upload(*device_, verts.data(), verts.size() * sizeof(glm::vec3));
    icosphereIdx_ = std::make_unique<Buffer>();
    icosphereIdx_->create(*device_, indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    icosphereIdx_->upload(*device_, indices.data(), indices.size() * sizeof(uint32_t));
    verts.clear();
    indices.clear();
    generateCylinderImpl(verts, indices);
    cylinderVerts_ = static_cast<uint32_t>(verts.size());
    cylinderIndices_ = static_cast<uint32_t>(indices.size());
    cylinder_ = std::make_unique<Buffer>();
    cylinder_->create(*device_, verts.size() * sizeof(glm::vec3),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    cylinder_->upload(*device_, verts.data(), verts.size() * sizeof(glm::vec3));
    cylinderIdx_ = std::make_unique<Buffer>();
    cylinderIdx_->create(*device_, indices.size() * sizeof(uint32_t),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    cylinderIdx_->upload(*device_, indices.data(), indices.size() * sizeof(uint32_t));

    drawBuffers_ = std::make_unique<DrawBuffers>();
    initialized_ = true;
    spdlog::info("[backend] OffscreenRenderer initialized ({}x{})", width_, height_);
    return true;
}

void OffscreenRenderer::shutdown() {
    if (!initialized_) {
        return;
    }
    vkDeviceWaitIdle(device_->device());
    drawBuffers_.reset();
    icosphere_.reset();
    icosphereIdx_.reset();
    cylinder_.reset();
    cylinderIdx_.reset();
    spherePipeline_.destroy(*device_);
    sphereWire_.destroy(*device_);
    sphereBlend_.destroy(*device_);
    cylinderPipeline_.destroy(*device_);
    cylinderWire_.destroy(*device_);
    cylinderBlend_.destroy(*device_);
    meshPipeline_.destroy(*device_);
    meshWire_.destroy(*device_);
    meshBlend_.destroy(*device_);
    linePipeline_.destroy(*device_);
    point_.destroy(*device_);
    globalUbo_.destroy();
    globalLayout_.destroy();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    destroyFramebuffer();
    renderPass_.destroy();   // 渲染通道在 shutdown 统一销毁（管线已先销毁）
    if (readbackBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_->device(), readbackBuffer_, nullptr);
        vkFreeMemory(device_->device(), readbackMemory_, nullptr);
        readbackBuffer_ = VK_NULL_HANDLE;
        readbackMemory_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
    ownedDevice_.reset();   // 仅 headless 自建设备；共享模式为空（主设备外部持有）
    device_ = nullptr;
}

void OffscreenRenderer::setBackground(const float* rgb) {
    clearColor_[0] = rgb[0];
    clearColor_[1] = rgb[1];
    clearColor_[2] = rgb[2];
    clearColor_[3] = 1.0f;
}

bool OffscreenRenderer::createFramebuffer() {
    destroyFramebuffer();
    // 无 MSAA（主方案 §12.5：导出 MSAA 可配，M1 先 1x；M4 支持 msaa 参数）
    const VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    if (!colorImage_.create(*device_, colorFormat, width_, height_, samples,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    if (!depthImage_.create(*device_, depthFormat, width_, height_, samples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        return false;
    }
    // 渲染通道不依赖视口尺寸（仅附件格式/采样数/布局），只在首次创建。
    // 若 resize 时重建渲染通道，initCommon 创建的管线（引用旧通道）会失效
    // → draw 无片元 → 离屏图像空白（清屏仍生效，掩盖 draw 失败）。
    if (renderPass_.renderPass() == VK_NULL_HANDLE) {
        if (!renderPass_.createOffscreen(*device_, colorFormat, depthFormat, samples,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            return false;
        }
    }
    if (!framebuffer_.create(*device_, renderPass_.renderPass(),
                             {colorImage_.view(), depthImage_.view()},
                             {width_, height_})) {
        return false;
    }
    // 采样器（线性，ImGui::Image 用）
    if (sampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 1.0f;
        vkCreateSampler(device_->device(), &info, nullptr, &sampler_);
    }
    colorLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void OffscreenRenderer::destroyFramebuffer() {
    framebuffer_.destroy();
    // 渲染通道被管线引用，须保持到 shutdown 才销毁；resize 重建图像/framebuffer
    // 时保留渲染通道（若销毁，管线失效 → 离屏绘制空白）。
    depthImage_.destroy();
    colorImage_.destroy();
}

void OffscreenRenderer::resize(uint32_t width, uint32_t height) {
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    vkDeviceWaitIdle(device_->device());
    destroyFramebuffer();
    createFramebuffer();
}

// —— 渲染 ——

void OffscreenRenderer::execute(const SceneCommandList& list, const RenderView& view) {
    // 队列提交互斥：后台导出线程与主线程视口共用同一 graphics 队列 → 串行提交
    std::unique_lock<std::mutex> qlock = device_->lockQueue();
    auto& buf = *drawBuffers_;
    buf.spheres.clear();
    buf.sphereMat.clear();
    buf.sphereMode.clear();
    buf.cylinders.clear();
    buf.cylinderMat.clear();
    buf.cylinderMode.clear();
    buf.lines.clear();
    buf.lineMat.clear();
    buf.meshes.clear();
    buf.materials.clear();

    uint32_t currentMat = 0;
    uint8_t currentMode = 0;   // 体系渲染模式（0实心/1线框/2透明/3点）
    buf.materials.push_back(MaterialBlock{});
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint8_t* payload = it.payload();
        const uint32_t size = it.payloadSize();
        switch (it.code()) {
            case Cmd::MaterialBlock: {
                if (size >= sizeof(MaterialBlock)) {
                    MaterialBlock mat;
                    std::memcpy(&mat, payload, sizeof(MaterialBlock));
                    buf.materials.push_back(mat);
                    currentMat = static_cast<uint32_t>(buf.materials.size()) - 1;
                }
                break;
            }
            case Cmd::RenderMode: {
                if (size >= sizeof(RenderModeInst)) {
                    RenderModeInst rmi;
                    std::memcpy(&rmi, payload, sizeof(RenderModeInst));
                    currentMode = rmi.mode;
                }
                break;
            }
            case Cmd::SphereInst: {
                if (size >= sizeof(SphereInst) && size % sizeof(SphereInst) == 0) {
                    const uint32_t n = size / sizeof(SphereInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        SphereInst inst;
                        std::memcpy(&inst, payload + i * sizeof(SphereInst),
                                    sizeof(SphereInst));
                        buf.spheres.push_back(inst);
                        buf.sphereMat.push_back(currentMat);
                        buf.sphereMode.push_back(currentMode);
                    }
                }
                break;
            }
            case Cmd::CylinderInst: {
                if (size >= sizeof(CylinderInst) && size % sizeof(CylinderInst) == 0) {
                    const uint32_t n = size / sizeof(CylinderInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        CylinderInst inst;
                        std::memcpy(&inst, payload + i * sizeof(CylinderInst),
                                    sizeof(CylinderInst));
                        buf.cylinders.push_back(inst);
                        buf.cylinderMat.push_back(currentMat);
                        buf.cylinderMode.push_back(currentMode);
                    }
                }
                break;
            }
            case Cmd::LineInst: {
                if (size >= sizeof(LineInst) && size % sizeof(LineInst) == 0) {
                    const uint32_t n = size / sizeof(LineInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        LineInst inst;
                        std::memcpy(&inst, payload + i * sizeof(LineInst),
                                    sizeof(LineInst));
                        buf.lines.push_back(inst);
                        buf.lineMat.push_back(currentMat);
                    }
                }
                break;
            }
            case Cmd::TriMesh: {
                if (size >= sizeof(TriMeshHeader)) {
                    TriMeshHeader hdr;
                    std::memcpy(&hdr, payload, sizeof(TriMeshHeader));
                    const uint32_t vertBytes = hdr.vertexCount * sizeof(TriMeshVertex);
                    const uint32_t idxBytes = hdr.indexCount * sizeof(uint32_t);
                    if (sizeof(TriMeshHeader) + vertBytes + idxBytes <= size) {
                        DrawBuffers::Mesh mesh;
                        mesh.mat = currentMat;
                        mesh.mode = currentMode;
                        mesh.verts.resize(hdr.vertexCount);
                        mesh.idx.resize(hdr.indexCount);
                        std::memcpy(mesh.verts.data(), payload + sizeof(TriMeshHeader),
                                    vertBytes);
                        std::memcpy(mesh.idx.data(),
                                    payload + sizeof(TriMeshHeader) + vertBytes, idxBytes);
                        buf.meshes.push_back(std::move(mesh));
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // UBO
    GlobalUBO ubo{};
    // vkProjection 的 Y 翻转/深度调整须作用于"纯投影矩阵"，再乘视图矩阵。
    // 直接作用于 P*V 联合矩阵只翻 m[1][1]，会把视图的 up 分量翻错 → clip.y 错误、分子不可见。
    ubo.viewProj = vkProjection(view.camera.projectionMatrix(
        static_cast<float>(width_) / static_cast<float>(height_))) *
                   view.camera.viewMatrix();
    ubo.cameraPos = glm::vec4(view.camera.position(), 1.0f);
    // 全局光照从 RenderView 读取（天顶/地面半球色 + 环境光；无方向光/点光源，
    // 见 moleculens-fixes —— 光照可调 + 更新机制经 DIRTY_PROPERTIES 重渲染）
    ubo.lightDir = glm::vec4(view.light.skyColor, 0.0f);
    ubo.lightColor = glm::vec4(view.light.groundColor, 0.0f);
    ubo.ambient = glm::vec4(view.light.ambientColor, view.light.ambientIntensity);
    ubo.fog = view.fog;
    ubo.sceneProps = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    globalUbo_.update(0, ubo);

    // 诊断（几何数量变化时）：确认离屏渲染确实执行且带几何
    {
        static uint32_t sLast = UINT32_MAX;
        const uint32_t total = static_cast<uint32_t>(buf.spheres.size() +
                                                     buf.cylinders.size() +
                                                     buf.lines.size() +
                                                     buf.meshes.size());
        if (total != sLast) {
            sLast = total;
            spdlog::info("[backend] offscreen execute: {} spheres, {} cylinders, {} lines, {} meshes @ {}x{}",
                         buf.spheres.size(), buf.cylinders.size(), buf.lines.size(),
                         buf.meshes.size(), width_, height_);
        }
    }

    VkCommandBuffer cmd = device_->beginSingleTimeCommands();
    VkViewport viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_),
                        0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width_, height_}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkClearValue clears[] = {
        {clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]},
        {1.0f, 0},
    };
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_.renderPass();
    rpBegin.framebuffer = framebuffer_.framebuffer();
    rpBegin.renderArea = scissor;
    rpBegin.clearValueCount = 2;   // color + depth
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkDescriptorSet globalSet = globalUbo_.set(0);
    // 体系渲染模式 → VkPipeline（实心/线框/透明/点；见 SystemRenderMode）
    const Pipeline* spherePipes[4] = {&spherePipeline_, &sphereWire_, &sphereBlend_, &point_};
    if (!buf.spheres.empty()) {
        drawModeRuns(cmd, buf.spheres, buf.sphereMat, buf.sphereMode, buf.materials,
                     buf.sphereInst, *icosphere_, *icosphereIdx_, icosphereIndices_,
                     spherePipes, globalSet);
    }
    if (!buf.cylinders.empty()) {
        // 点模式键 → 线段（用 line 管线）；其余模式用对应圆柱管线
        const Pipeline* cylPipes[4] = {&cylinderPipeline_, &cylinderWire_, &cylinderBlend_,
                                       &cylinderPipeline_};
        std::vector<CylinderInst> cylNonPoint;
        std::vector<uint32_t> cylMatNonPoint;
        std::vector<uint8_t> cylModeNonPoint;
        std::vector<glm::vec4> pointLineVerts;   // a,color,b,color（点模式键线段）
        for (size_t ci = 0; ci < buf.cylinders.size(); ++ci) {
            if (buf.cylinderMode[ci] == 3) {
                const CylinderInst& c = buf.cylinders[ci];
                pointLineVerts.push_back(c.aPos);
                pointLineVerts.push_back(c.color);
                pointLineVerts.push_back(c.bPos);
                pointLineVerts.push_back(c.color);
            } else {
                cylNonPoint.push_back(buf.cylinders[ci]);
                cylMatNonPoint.push_back(buf.cylinderMat[ci]);
                cylModeNonPoint.push_back(buf.cylinderMode[ci]);
            }
        }
        if (!cylNonPoint.empty()) {
            drawModeRuns(cmd, cylNonPoint, cylMatNonPoint, cylModeNonPoint, buf.materials,
                         buf.cylinderInst, *cylinder_, *cylinderIdx_, cylinderIndices_,
                         cylPipes, globalSet);
        }
        if (!pointLineVerts.empty()) {
            const VkDeviceSize need = pointLineVerts.size() * sizeof(glm::vec4);
            if (!buf.lineVerts.valid() || buf.lineVerts.size() < need) {
                buf.lineVerts.create(*device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            buf.lineVerts.upload(*device_, pointLineVerts.data(), need);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    linePipeline_.layout(), 0, 1, &globalSet, 0, nullptr);
            // line.frag 直接用顶点色（无材质），push 默认材质即可
            const MaterialBlock& lmat = buf.materials.empty() ? MaterialBlock{}
                                                              : buf.materials[0];
            vkCmdPushConstants(cmd, linePipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MaterialBlock), &lmat);
            VkBuffer vb = buf.lineVerts.buffer();
            const VkDeviceSize offset = 0;
            // pOffsets 必须是非 NULL 有效指针（传 nullptr 是校验错误 → 驱动崩溃 SIGSEGV）
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdDraw(cmd, static_cast<uint32_t>(pointLineVerts.size() / 2), 1, 0, 0);
        }
    }
    if (!buf.lines.empty()) {
        std::vector<glm::vec4> lineVerts;
        lineVerts.reserve(buf.lines.size() * 4);
        for (const auto& l : buf.lines) {
            lineVerts.push_back(l.aPos);
            lineVerts.push_back(l.color);
            lineVerts.push_back(l.bPos);
            lineVerts.push_back(l.color);
        }
        const VkDeviceSize need = lineVerts.size() * sizeof(glm::vec4);
        if (!buf.lineVerts.valid() || buf.lineVerts.size() < need) {
            buf.lineVerts.create(*device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        }
        buf.lineVerts.upload(*device_, lineVerts.data(), need);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                linePipeline_.layout(), 0, 1, &globalSet, 0, nullptr);
        const MaterialBlock mat = buf.materials.empty() ? MaterialBlock{} : buf.materials[0];
        vkCmdPushConstants(cmd, linePipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(MaterialBlock), &mat);
        VkBuffer vb = buf.lineVerts.buffer();
        const VkDeviceSize offset = 0;
        // pOffsets 必须是非 NULL 有效指针（传 nullptr 是校验错误 → 驱动崩溃 SIGSEGV，
        // 线框模式因此无法显示，见 moleculens-fixes）
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(lineVerts.size() / 2), 1, 0, 0);
    }
    if (!buf.meshes.empty()) {
        // 按体系渲染模式分组合并绘制（实心/线框/透明；点模式网格按实心降级）
        for (int mode = 0; mode < 4; ++mode) {
            size_t totalVerts = 0, totalIdx = 0;
            for (const auto& mesh : buf.meshes) {
                if (mesh.mode == mode) {
                    totalVerts += mesh.verts.size();
                    totalIdx += mesh.idx.size();
                }
            }
            if (totalVerts == 0 || totalIdx == 0) {
                continue;
            }
            const Pipeline& mp = (mode == 1) ? meshWire_
                                  : (mode == 2 ? meshBlend_ : meshPipeline_);
            std::vector<TriMeshVertex> allVerts;
            std::vector<uint32_t> allIdx;
            allVerts.reserve(totalVerts);
            allIdx.reserve(totalIdx);
            for (const auto& mesh : buf.meshes) {
                if (mesh.mode != mode) {
                    continue;
                }
                const uint32_t base = static_cast<uint32_t>(allVerts.size());
                // 把网格材质（diffuse/opacity）烘焙进顶点色；片元 shader 用
                // mat.matDiffuse*vColor，若硬编码 buf.materials[0]（默认全 0）
                // 网格将全黑不可见（表面/卡通模式根因之一）。
                const MaterialBlock& mm = buf.materials[mesh.mat < buf.materials.size()
                                                             ? mesh.mat
                                                             : 0];
                for (const auto& v : mesh.verts) {
                    TriMeshVertex tv = v;
                    // 逐分量乘（避免 GLM swizzle；本工程 GLM 未开 GLM_FORCE_SWIZZLE）
                    tv.color = glm::vec4(tv.color.r * mm.diffuse.x,
                                         tv.color.g * mm.diffuse.y,
                                         tv.color.b * mm.diffuse.z,
                                         tv.color.a * mm.opacity);
                    allVerts.push_back(tv);
                }
                for (uint32_t i : mesh.idx) {
                    allIdx.push_back(base + i);
                }
            }
            const VkDeviceSize vertNeed = allVerts.size() * sizeof(TriMeshVertex);
            if (!buf.meshVerts.valid() || buf.meshVerts.size() < vertNeed) {
                buf.meshVerts.create(*device_, vertNeed, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            buf.meshVerts.upload(*device_, allVerts.data(), vertNeed);
            const VkDeviceSize idxNeed = allIdx.size() * sizeof(uint32_t);
            if (!buf.meshIdx.valid() || buf.meshIdx.size() < idxNeed) {
                buf.meshIdx.create(*device_, idxNeed, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            buf.meshIdx.upload(*device_, allIdx.data(), idxNeed);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.layout(), 0, 1,
                                    &globalSet, 0, nullptr);
            // 漫反射已烘焙进顶点色，push 白色默认材质（diffuse=1）避免二次变暗
            MaterialBlock mat;
            mat.ambient = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f);
            mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            mat.specular = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);
            mat.shininess = 32.0f;
            mat.metallic = 0.0f;
            mat.roughness = 0.4f;
            mat.opacity = 1.0f;
            vkCmdPushConstants(cmd, mp.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MaterialBlock), &mat);
            VkBuffer vb = buf.meshVerts.buffer();
            const VkDeviceSize meshOffset = 0;
            // pOffsets 必须非 NULL（同线段 draw：传 nullptr 校验错误 + SIGSEGV）
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &meshOffset);
            vkCmdBindIndexBuffer(cmd, buf.meshIdx.buffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, static_cast<uint32_t>(allIdx.size()), 1, 0, 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
    device_->endSingleTimeCommands(cmd);
}

// 按 (体系渲染模式, 材质) 连续批次绘制实例化几何。每个模式对应不同 VkPipeline
// （pipes[4]：0实心/1线框/2透明/3点）。点模式用 GL_POINTS（vkCmdDraw 1 顶点/实例）。
template <typename InstT>
void OffscreenRenderer::drawModeRuns(
    VkCommandBuffer cmd, const std::vector<InstT>& instances,
    const std::vector<uint32_t>& mats, const std::vector<uint8_t>& modes,
    const std::vector<MaterialBlock>& materials, Buffer& instanceBuffer,
    const Buffer& mesh, const Buffer& meshIdx, uint32_t indexCount,
    const Pipeline* pipes[4], VkDescriptorSet set) {
    if (instances.empty()) {
        return;
    }
    const VkDeviceSize need = instances.size() * sizeof(InstT);
    if (!instanceBuffer.valid() || instanceBuffer.size() < need) {
        instanceBuffer.create(*device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    }
    instanceBuffer.upload(*device_, instances.data(), need);

    uint32_t runStart = 0;
    uint32_t runMat = mats.empty() ? 0 : mats[0];
    uint8_t runMode = modes.empty() ? 0 : modes[0];
    const auto drawRun = [&](uint32_t start, uint32_t count, uint32_t mode,
                             uint32_t matIdx) {
        const Pipeline& p = *pipes[mode % 4];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0, 1,
                                &set, 0, nullptr);
        const MaterialBlock& mat = materials[matIdx];
        vkCmdPushConstants(cmd, p.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(MaterialBlock), &mat);
        if (mode == 3) {
            // 点模式：GL_POINTS（point.vert 无顶点绑定；仅实例缓冲 binding 1）
            VkBuffer vb = instanceBuffer.buffer();
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 1, 1, &vb, &off);
            vkCmdDraw(cmd, 1, count, 0, start);
        } else {
            VkBuffer vbs[] = {mesh.buffer(), instanceBuffer.buffer()};
            VkDeviceSize offs[] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            vkCmdBindIndexBuffer(cmd, meshIdx.buffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, indexCount, count, 0, 0, start);
        }
    };
    for (size_t i = 1; i < instances.size(); ++i) {
        const uint32_t m = mats.empty() ? 0 : mats[i];
        const uint8_t mode = modes.empty() ? 0 : modes[i];
        if (m != runMat || mode != runMode) {
            drawRun(runStart, static_cast<uint32_t>(i - runStart), runMode, runMat);
            runStart = static_cast<uint32_t>(i);
            runMat = m;
            runMode = mode;
        }
    }
    drawRun(runStart, static_cast<uint32_t>(instances.size() - runStart), runMode,
            runMat);
}

bool OffscreenRenderer::readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt) {
    // 队列提交互斥（与 execute 一致，见 QS.4）
    std::unique_lock<std::mutex> qlock = device_->lockQueue();
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ *
                               (fmt == PixelFormat::Rgba16F ? 8 : 4);
    VkCommandBuffer cmd = device_->beginSingleTimeCommands();

    // colorImage → readback buffer
    if (readbackBuffer_ == VK_NULL_HANDLE || readbackSize_ < bytes) {
        if (readbackBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_->device(), readbackBuffer_, nullptr);
            vkFreeMemory(device_->device(), readbackMemory_, nullptr);
        }
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device_->device(), &info, nullptr, &readbackBuffer_);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_->device(), readbackBuffer_, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(device_->physicalDevice(), &props);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                alloc.memoryTypeIndex = i;
                break;
            }
        }
        vkAllocateMemory(device_->device(), &alloc, nullptr, &readbackMemory_);
        vkBindBufferMemory(device_->device(), readbackBuffer_, readbackMemory_, 0);
        readbackSize_ = bytes;
    }

    // 布局转换：SHADER_READ_ONLY → TRANSFER_SRC（拷贝后转回）
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = colorImage_.image();
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(cmd, colorImage_.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_, 1, &region);

    // 转回 SHADER_READ_ONLY（供 ImGui 采样）
    VkImageMemoryBarrier back{};
    back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back.image = colorImage_.image();
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &back);
    device_->endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    vkMapMemory(device_->device(), readbackMemory_, 0, bytes, 0, &mapped);
    if (!mapped) {
        return false;
    }
    // GPU 已把图像拷入 readbackBuffer_；若该内存类型非 HOST_COHERENT，须显式
    // invalidate 使主机端缓存失效，否则读到的是陈旧数据（全背景/垃圾）→ 误判渲染空白。
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readbackMemory_;
    range.offset = 0;
    range.size = bytes;
    vkInvalidateMappedMemoryRanges(device_->device(), 1, &range);
    out.assign(static_cast<const uint8_t*>(mapped),
               static_cast<const uint8_t*>(mapped) + bytes);
    pixels_ = out;
    vkUnmapMemory(device_->device(), readbackMemory_);
    return true;
}

} // namespace moleculens::engine
