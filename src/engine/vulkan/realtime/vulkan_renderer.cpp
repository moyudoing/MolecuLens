// =============================================================================
// MolecuLens · engine · vulkan/realtime/vulkan_renderer.cpp —— Vulkan 渲染器实现
// 规格来源：主方案 §6.1/§6.3/§6.4（初始化流程；帧循环；命令流 → 缓冲编译；
//           共享网格 icosphere/圆柱；材质 push constant）
// =============================================================================

#include "engine/vulkan/realtime/vulkan_renderer.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <unordered_map>

#include <GLFW/glfw3.h>

#include "spdlog/spdlog.h"

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"

namespace moleculens::engine {

namespace {

// 生成头（cmake shaders.cmake 内嵌）
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

// 生成单位 icosphere（细分 2 级 → 320 三角形，主方案 §5.7.1；比 80 更平滑）
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

void generateIcosphere(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    // 正二十面体
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
        const uint32_t i0 = f[0], i1 = f[1], i2 = f[2];
        const glm::vec3 v0 = verts[i0], v1 = verts[i1], v2 = verts[i2];
        const glm::vec3 m01 = glm::normalize(v0 + v1);
        const glm::vec3 m12 = glm::normalize(v1 + v2);
        const glm::vec3 m20 = glm::normalize(v2 + v0);
        const uint32_t j0 = static_cast<uint32_t>(verts.size());
        const uint32_t j1 = j0 + 1, j2 = j0 + 2;
        verts.push_back(m01);
        verts.push_back(m12);
        verts.push_back(m20);
        indices.insert(indices.end(), {i0, j0, j2, j0, i1, j1, j2, j1, i2, j0, j1, j2});
    }
    subdivideOnce(verts, indices);   // 2 级：80 → 320 三角形
}

// 生成单位圆柱（8 段，半径 1，y∈[-1,1]，无端盖，主方案 §5.7.3）
void generateCylinder(std::vector<glm::vec3>& verts, std::vector<uint32_t>& indices) {
    constexpr int kSegments = 8;
    const uint32_t base = static_cast<uint32_t>(verts.size());
    for (int i = 0; i < kSegments; ++i) {
        const float a = 6.2831853f * i / kSegments;
        const glm::vec3 p(std::cos(a), 0.0f, std::sin(a));
        verts.push_back(glm::vec3(p.x, -1.0f, p.z));
        verts.push_back(glm::vec3(p.x, 1.0f, p.z));
    }
    for (int i = 0; i < kSegments; ++i) {
        const uint32_t i0 = base + static_cast<uint32_t>(i) * 2;
        const uint32_t i1 = i0 + 1;
        const uint32_t i2 = base + static_cast<uint32_t>((i + 1) % kSegments) * 2;
        const uint32_t i3 = i2 + 1;
        indices.insert(indices.end(), {i0, i1, i2, i1, i3, i2});
    }
}

// Vulkan 深度范围 [0,1] + y 翻转（OpenGL → Vulkan 坐标修正）
glm::mat4 vkProjection(const glm::mat4& proj) {
    glm::mat4 m = proj;
    m[1][1] *= -1.0f;
    // 深度范围：glm::perspective 默认 [-1,1] → 改为 [0,1]。
    // 修正索引：GLM 列主序中 z 行 w 系数在 m[3][2]，w 行 z 系数在 m[2][3]。
    // 此前读取/写入方向相反 → clip.w 变 -0.5z、深度错误 → 几何被远平面裁剪。
    m[2][2] = m[2][2] * 0.5f + m[2][3] * 0.5f;
    m[3][2] = m[3][2] * 0.5f;
    return m;
}

} // namespace

// —— 每帧绘制缓冲（定义于头文件：跨 TU 析构需完整类型）——

VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::init(WindowHandle window, const RenderConfig& cfg,
                          std::string* outError) {
    shutdown();
    config_ = cfg;
    if (!device_.init(window, cfg, outError)) {
        return false;
    }
    if (!device_.createSwapchain()) {
        if (outError) *outError = "createSwapchain failed";
        return false;
    }
    // 主渲染通道
    if (!mainRenderPass_.createMain(device_, device_.swapchainFormat(),
                                    device_.depthFormat(), device_.msaaSamples())) {
        return false;
    }
    // 全局描述符（UBO）
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!globalLayout_.create(device_, {binding})) {
        return false;
    }
    if (!globalUbo_.create(device_, globalLayout_.layout(), sizeof(GlobalUBO))) {
        return false;
    }
    if (!createPipelines()) {
        return false;
    }
    createSharedMeshes();
    createImGuiPool();
    if (!createMainFramebuffers()) {
        return false;
    }
    drawBuffers_ = std::make_unique<DrawBuffers>();
    initialized_ = true;
    spdlog::info("[backend] VulkanRenderer initialized ({}x{}, msaa={})",
                 device_.swapchainExtent().width, device_.swapchainExtent().height,
                 static_cast<uint32_t>(device_.msaaSamples()));
    return true;
}

void VulkanRenderer::shutdown() {
    if (!initialized_) {
        return;
    }
    vkDeviceWaitIdle(device_.device());
    destroyPickResources();
    drawBuffers_.reset();
    icosphere_.reset();
    icosphereIdx_.reset();
    cylinder_.reset();
    cylinderIdx_.reset();
    spherePipeline_.destroy(device_);
    sphereWire_.destroy(device_);
    sphereBlend_.destroy(device_);
    cylinderPipeline_.destroy(device_);
    cylinderWire_.destroy(device_);
    cylinderBlend_.destroy(device_);
    meshPipeline_.destroy(device_);
    meshWire_.destroy(device_);
    meshBlend_.destroy(device_);
    linePipeline_.destroy(device_);
    point_.destroy(device_);
    delete sphereVert_;
    delete sphereFrag_;
    delete cylinderVert_;
    delete cylinderFrag_;
    delete lineVert_;
    delete lineFrag_;
    delete meshVert_;
    delete meshFrag_;
    delete pointVert_;
    delete pointFrag_;
    sphereVert_ = sphereFrag_ = nullptr;
    cylinderVert_ = cylinderFrag_ = nullptr;
    lineVert_ = lineFrag_ = nullptr;
    meshVert_ = meshFrag_ = nullptr;
    pointVert_ = pointFrag_ = nullptr;
    globalUbo_.destroy();
    globalLayout_.destroy();
    destroyMainFramebuffers();
    mainRenderPass_.destroy();
    // imguiPool_ 所有权归 ImGui_ImplVulkan 后端（其 Shutdown 会销毁传入的 pool，
    // 见 imgui_impl_vulkan.cpp Shutdown）；此处不得重复销毁（双重释放会崩溃）。
    imguiPool_ = VK_NULL_HANDLE;
    device_.destroySwapchain();
    initialized_ = false;
}

// —— 主渲染附件与帧缓冲 ——
bool VulkanRenderer::createMainFramebuffers() {
    destroyMainFramebuffers();
    const VkExtent2D extent = device_.swapchainExtent();
    const VkFormat colorFormat = device_.swapchainFormat();
    const VkSampleCountFlagBits samples = device_.msaaSamples();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    if (msaa) {
        if (!msaaImage_.create(device_, colorFormat, extent.width, extent.height,
                               samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            return false;
        }
    }
    if (!depthImage_.create(device_, device_.depthFormat(), extent.width,
                            extent.height, samples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        return false;
    }
    swapchainFramebuffers_.resize(device_.swapchainViews().size());
    for (size_t i = 0; i < device_.swapchainViews().size(); ++i) {
        std::vector<VkImageView> attachments;
        if (msaa) {
            attachments.push_back(msaaImage_.view());
            attachments.push_back(depthImage_.view());
            attachments.push_back(device_.swapchainViews()[i]);  // resolve
        } else {
            attachments.push_back(device_.swapchainViews()[i]);
            attachments.push_back(depthImage_.view());
        }
        if (!swapchainFramebuffers_[i].create(device_, mainRenderPass_.renderPass(),
                                              attachments, extent)) {
            return false;
        }
    }
    spdlog::info("[backend] main framebuffers created ({}x{}, msaa={})",
                 extent.width, extent.height, static_cast<uint32_t>(samples));
    return true;
}

void VulkanRenderer::destroyMainFramebuffers() {
    for (auto& fb : swapchainFramebuffers_) {
        fb.destroy();
    }
    swapchainFramebuffers_.clear();
    depthImage_.destroy();
    msaaImage_.destroy();
}

bool VulkanRenderer::createPipelines() {
    auto loadShader = [&](ShaderModule*& out, VkShaderStageFlagBits stage,
                          const uint8_t* spv, size_t size) -> bool {
        out = new ShaderModule();
        return out->create(device_, stage, spv, size);
    };
    if (!loadShader(sphereVert_, VK_SHADER_STAGE_VERTEX_BIT, sphere_vert, sphere_vert_len) ||
        !loadShader(sphereFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, sphere_frag, sphere_frag_len) ||
        !loadShader(cylinderVert_, VK_SHADER_STAGE_VERTEX_BIT, cylinder_vert, cylinder_vert_len) ||
        !loadShader(cylinderFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, cylinder_frag, cylinder_frag_len) ||
        !loadShader(lineVert_, VK_SHADER_STAGE_VERTEX_BIT, line_vert, line_vert_len) ||
        !loadShader(lineFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, line_frag, line_frag_len) ||
        !loadShader(meshVert_, VK_SHADER_STAGE_VERTEX_BIT, mesh_vert, mesh_vert_len) ||
        !loadShader(meshFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, mesh_frag, mesh_frag_len) ||
        !loadShader(pointVert_, VK_SHADER_STAGE_VERTEX_BIT, point_vert, point_vert_len) ||
        !loadShader(pointFrag_, VK_SHADER_STAGE_FRAGMENT_BIT, point_frag, point_frag_len)) {
        return false;
    }

    const VkSampleCountFlagBits samples = device_.msaaSamples();
    const VkRenderPass rp = mainRenderPass_.renderPass();

    // 每体系渲染模式一套管线（实心/线框/透明/点；见 SystemRenderMode）
    struct PipeSpec {
        const VkPipelineShaderStageCreateInfo* stages = nullptr;
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
        return pb.build(device_, out);
    };

    // —— sphere 管线：binding0 = mesh 顶点(vec3)，binding1 = SphereInst ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {
            sphereVert_->stage(), sphereFrag_->stage(),
        };
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
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL}, &spherePipeline_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE}, &sphereWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL}, &sphereBlend_)) {
            return false;
        }
    }

    // —— cylinder 管线：binding1 = CylinderInst ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {
            cylinderVert_->stage(), cylinderFrag_->stage(),
        };
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
        if (!buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_FILL}, &cylinderPipeline_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE}, &cylinderWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL}, &cylinderBlend_)) {
            return false;
        }
    }

    // —— line 管线：binding0 = 顶点(pos4+color4 交错 = 32B) ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {
            lineVert_->stage(), lineFrag_->stage(),
        };
        // 顶点布局 = pos(float4) + color(float4) = 32B/顶点（line.vert）；
        // 原 48B stride 与数据不匹配 → 取数错位 → 线框渲染错乱。
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
                        VK_POLYGON_MODE_FILL}, &linePipeline_)) {
            return false;
        }
    }

    // —— mesh 管线（TriMesh）：binding0 = TriMeshVertex 48B ——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {
            meshVert_->stage(), meshFrag_->stage(),
        };
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
                        VK_POLYGON_MODE_FILL}, &meshPipeline_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, true,
                        VK_POLYGON_MODE_LINE}, &meshWire_) ||
            !buildPipe({stages, b, a, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, false,
                        VK_POLYGON_MODE_FILL}, &meshBlend_)) {
            return false;
        }
    }

    // —— point 管线（GL_POINTS；SphereInst 实例数据，仅 binding 1）——
    {
        const VkPipelineShaderStageCreateInfo stages[] = {
            pointVert_->stage(), pointFrag_->stage(),
        };
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
                        VK_POLYGON_MODE_FILL}, &point_)) {
            return false;
        }
    }
    return true;
}

void VulkanRenderer::createSharedMeshes() {
    // icosphere（主方案 §5.7.1：细分 2 级，80 三角形）
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> indices;
    generateIcosphere(verts, indices);
    icosphereVerts_ = static_cast<uint32_t>(verts.size());
    icosphereIndices_ = static_cast<uint32_t>(indices.size());
    icosphere_ = std::make_unique<Buffer>();
    icosphere_->create(device_, verts.size() * sizeof(glm::vec3),
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    icosphere_->upload(device_, verts.data(), verts.size() * sizeof(glm::vec3));
    icosphereIdx_ = std::make_unique<Buffer>();
    icosphereIdx_->create(device_, indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    icosphereIdx_->upload(device_, indices.data(), indices.size() * sizeof(uint32_t));

    // 圆柱（8 段，主方案 §5.7.3）
    verts.clear();
    indices.clear();
    generateCylinder(verts, indices);
    cylinderVerts_ = static_cast<uint32_t>(verts.size());
    cylinderIndices_ = static_cast<uint32_t>(indices.size());
    cylinder_ = std::make_unique<Buffer>();
    cylinder_->create(device_, verts.size() * sizeof(glm::vec3),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    cylinder_->upload(device_, verts.data(), verts.size() * sizeof(glm::vec3));
    cylinderIdx_ = std::make_unique<Buffer>();
    cylinderIdx_->create(device_, indices.size() * sizeof(uint32_t),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    cylinderIdx_->upload(device_, indices.data(), indices.size() * sizeof(uint32_t));
}

void VulkanRenderer::createImGuiPool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 100},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100},
    };
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = 1000;
    info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_.device(), &info, nullptr, &imguiPool_);
}

// —— 帧循环 ——

void VulkanRenderer::beginFrame() {
    // 阶段跟踪（每 60 帧）：B=beginFrame, R=record, E=endFrame
    static uint32_t s_tick = 0;
    if ((s_tick++ % 60) == 0) {
    }
    // 等待 fence（该帧图像可用）
    vkWaitForFences(device_.device(), 1, &device_.inFlightFences()[frameIndex_],
                    VK_TRUE, UINT64_MAX);
    const uint32_t result = device_.acquireNextImage(
        device_.imageAvailable()[frameIndex_], &imageIndex_);
    if (result == 1) {
        // 交换链过期 → 重建帧缓冲并跳过本帧
        device_.recreateSwapchain();
        createMainFramebuffers();
        resized_ = false;
        return;
    }
    if (result == 2) {
        return;
    }
    vkResetFences(device_.device(), 1, &device_.inFlightFences()[frameIndex_]);
    // 上一帧同一槽位的命令缓冲此刻已由 fence 确认 GPU 使用完毕，可以安全释放
    //（endFrame 只结束记录，释放推迟到这里，避免提交已释放句柄）。
    if (frameCommandSlots_[frameIndex_] != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_.device(), device_.commandPool(), 1,
                             &frameCommandSlots_[frameIndex_]);
        frameCommandSlots_[frameIndex_] = VK_NULL_HANDLE;
    }
    frameCommand_ = device_.beginFrameCommands();
    frameCommandSlots_[frameIndex_] = frameCommand_;
}

void VulkanRenderer::execute(const SceneCommandList& list, const RenderView& view) {
    if (frameCommand_ == VK_NULL_HANDLE) {
        return;
    }
    recordFrame(list, view);
}

void VulkanRenderer::endFrame() {
    if (frameCommand_ == VK_NULL_HANDLE) {
        return;
    }
    // 队列提交互斥：与后台导出线程串行（QS.4 图像导出多线程）
    std::unique_lock<std::mutex> qlock = device_.lockQueue();
    device_.endFrameCommands(frameCommand_);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &device_.imageAvailable()[frameIndex_];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frameCommand_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &device_.renderFinished()[frameIndex_];

    const VkResult subResult = vkQueueSubmit(device_.graphicsQueue(), 1, &submit,
                      device_.inFlightFences()[frameIndex_]);
    if (subResult != VK_SUCCESS) {
        spdlog::error("[backend] vkQueueSubmit failed");
    }
    const VkResult presentResult =
        device_.presentImage(imageIndex_, device_.renderFinished()[frameIndex_]);
    (void)presentResult;   // M2：present 结果处理（OUT_OF_DATE → 重建交换链）

    frameIndex_ = (frameIndex_ + 1) % device_.maxFramesInFlight();
    frameCommand_ = VK_NULL_HANDLE;
}

void VulkanRenderer::resize(uint32_t w, uint32_t h) {
    (void)w; (void)h;
    resized_ = true;
}

// —— 命令流执行（主方案 §6.4）——

void VulkanRenderer::recordFrame(const SceneCommandList& list, const RenderView& view) {
    auto& buf = *drawBuffers_;
    buf.spheres.clear();
    buf.sphereMat.clear();
    buf.cylinders.clear();
    buf.cylinderMat.clear();
    buf.cylinderMode.clear();
    buf.lines.clear();
    buf.lineMat.clear();
    buf.meshes.clear();
    buf.materials.clear();
    buf.sphereMode.clear();

    // 收集命令流（13 种命令解包，主方案 §5.3/附录 F）
    uint32_t currentMat = 0;
    uint8_t currentMode = 0;   // 体系渲染模式（0实心/1线框/2透明/3点）
    buf.materials.push_back(MaterialBlock{});   // 默认材质（下标 0）
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
            case Cmd::Transform:
            case Cmd::PushMatrix:
            case Cmd::PopMatrix:
            case Cmd::ClipPlane:
            case Cmd::SetBlend:
            case Cmd::SetDepthWrite:
                // M2：变换栈/裁剪面/状态（当前忽略，主方案 §6.4 记录为渲染状态）
                break;
            case Cmd::Label:
            case Cmd::Count:
                break;
        }
    }

    // UBO 更新（相机 + 光照 + 场景属性，主方案 §6.6.4）
    GlobalUBO ubo{};
    // vkProjection 作用于纯投影矩阵（Y 翻转/深度调整），再乘视图矩阵；
    // 直接作用于 P*V 联合矩阵会只翻 m[1][1] → clip.y 错误、几何不可见。
    ubo.viewProj = vkProjection(view.camera.projectionMatrix(
        device_.swapchainExtent().width / static_cast<float>(
            std::max(1u, device_.swapchainExtent().height)))) *
                   view.camera.viewMatrix();
    ubo.cameraPos = glm::vec4(view.camera.position(), 1.0f);
    // 全局光照从 RenderView 读取（天顶/地面半球色 + 环境光；无方向光/点光源）
    ubo.lightDir = glm::vec4(view.light.skyColor, 0.0f);
    ubo.lightColor = glm::vec4(view.light.groundColor, 0.0f);
    ubo.ambient = glm::vec4(view.light.ambientColor, view.light.ambientIntensity);
    ubo.fog = view.fog;
    ubo.sceneProps = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    globalUbo_.update(frameIndex_, ubo);

    // —— 记录渲染 ——
    VkCommandBuffer cmd = frameCommand_;
    const VkExtent2D extent = device_.swapchainExtent();
    VkViewport viewport{0, 0, static_cast<float>(extent.width),
                        static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const VkClearValue clearColor = {0.05f, 0.05f, 0.08f, 1.0f};
    const VkClearValue clearDepth = {1.0f, 0};
    const VkClearValue clears[] = {clearColor, clearDepth};
    if (imageIndex_ >= swapchainFramebuffers_.size()) {
        return;
    }
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = mainRenderPass_.renderPass();
    rpBegin.framebuffer = swapchainFramebuffers_[imageIndex_].framebuffer();
    rpBegin.renderArea = scissor;
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkDescriptorSet globalSet = globalUbo_.set(frameIndex_);

    // 体系渲染模式 → VkPipeline（实心/线框/透明/点；见 SystemRenderMode）
    const Pipeline* spherePipes[4] = {&spherePipeline_, &sphereWire_, &sphereBlend_, &point_};
    // 球（实例化，按 (模式,材质) 分批）
    if (!buf.spheres.empty()) {
        drawModeRuns(buf.spheres, buf.sphereMat, buf.sphereMode, buf.materials,
                     buf.sphereInst, *icosphere_, *icosphereIdx_, icosphereIndices_,
                     spherePipes, globalSet);
    }
    // 圆柱（点模式键 → 线段）
    if (!buf.cylinders.empty()) {
        const Pipeline* cylPipes[4] = {&cylinderPipeline_, &cylinderWire_, &cylinderBlend_,
                                       &cylinderPipeline_};
        std::vector<CylinderInst> cylNonPoint;
        std::vector<uint32_t> cylMatNonPoint;
        std::vector<uint8_t> cylModeNonPoint;
        std::vector<glm::vec4> pointLineVerts;   // a,color,b,color
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
            drawModeRuns(cylNonPoint, cylMatNonPoint, cylModeNonPoint, buf.materials,
                         buf.cylinderInst, *cylinder_, *cylinderIdx_, cylinderIndices_,
                         cylPipes, globalSet);
        }
        if (!pointLineVerts.empty()) {
            const VkDeviceSize need = pointLineVerts.size() * sizeof(glm::vec4);
            if (!buf.lineVerts.valid() || buf.lineVerts.size() < need) {
                buf.lineVerts.create(device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            buf.lineVerts.upload(device_, pointLineVerts.data(), need);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    linePipeline_.layout(), 0, 1, &globalSet, 0, nullptr);
            const MaterialBlock& lmat = buf.materials.empty() ? MaterialBlock{}
                                                              : buf.materials[0];
            vkCmdPushConstants(cmd, linePipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MaterialBlock), &lmat);
            VkBuffer vb = buf.lineVerts.buffer();
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdDraw(cmd, static_cast<uint32_t>(pointLineVerts.size() / 2), 1, 0, 0);
        }
    }
    // 线段
    if (!buf.lines.empty()) {
        std::vector<glm::vec4> lineVerts;
        lineVerts.reserve(buf.lines.size() * 2 * 2);
        for (size_t i = 0; i < buf.lines.size(); ++i) {
            const LineInst& l = buf.lines[i];
            const MaterialBlock& mat = buf.materials[buf.lineMat[i]];
            const glm::vec4 col = l.color * glm::vec4(mat.diffuse) * glm::vec4(mat.opacity);
            lineVerts.push_back(l.aPos);
            lineVerts.push_back(col);
            lineVerts.push_back(l.bPos);
            lineVerts.push_back(col);
        }
        const VkDeviceSize need = lineVerts.size() * sizeof(glm::vec4);
        if (!buf.lineVerts.valid() || buf.lineVerts.size() < need) {
            buf.lineVerts.create(device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        }
        buf.lineVerts.upload(device_, lineVerts.data(), need);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                linePipeline_.layout(), 0, 1, &globalSet, 0, nullptr);
        const MaterialBlock mat = buf.materials.empty() ? MaterialBlock{} : buf.materials[0];
        vkCmdPushConstants(cmd, linePipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(MaterialBlock), &mat);
        VkBuffer vb = buf.lineVerts.buffer();
        const VkDeviceSize offset = 0;
        // pOffsets 必须非 NULL（否则校验错误 + 驱动崩溃 SIGSEGV，线框不可用）
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(lineVerts.size() / 2), 1, 0, 0);
    }
    // 三角网格（卡通/表面；按体系渲染模式分组合并绘制）
    if (!buf.meshes.empty()) {
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
                // 网格材质（diffuse/opacity）烘焙进顶点色，避免硬编码
                // buf.materials[0]（默认全 0）导致表面/卡通全黑。
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
                buf.meshVerts.create(device_, vertNeed, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            buf.meshVerts.upload(device_, allVerts.data(), vertNeed);
            const VkDeviceSize idxNeed = allIdx.size() * sizeof(uint32_t);
            if (!buf.meshIdx.valid() || buf.meshIdx.size() < idxNeed) {
                buf.meshIdx.create(device_, idxNeed, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            buf.meshIdx.upload(device_, allIdx.data(), idxNeed);
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

    // ImGui（UI 回调；renderPass 内）
    if (uiRenderFn_) {
        uiRenderFn_(cmd);
    }

    vkCmdEndRenderPass(cmd);
}

// 按 (体系渲染模式, 材质) 连续批次绘制实例化几何。每个模式对应不同 VkPipeline
// （pipes[4]：0实心/1线框/2透明/3点）。点模式用 GL_POINTS（vkCmdDraw 1 顶点/实例）。
template <typename InstT>
void VulkanRenderer::drawModeRuns(const std::vector<InstT>& instances,
                                  const std::vector<uint32_t>& mats,
                                  const std::vector<uint8_t>& modes,
                                  const std::vector<MaterialBlock>& materials,
                                  Buffer& instanceBuffer,
                                  const Buffer& mesh, const Buffer& meshIdx,
                                  uint32_t indexCount,
                                  const Pipeline* pipes[4], VkDescriptorSet set) {
    if (instances.empty()) {
        return;
    }
    VkCommandBuffer cmd = frameCommand_;
    const VkDeviceSize need = instances.size() * sizeof(InstT);
    if (!instanceBuffer.valid() || instanceBuffer.size() < need) {
        instanceBuffer.create(device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    }
    instanceBuffer.upload(device_, instances.data(), need);

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

bool VulkanRenderer::createPickResources() {
    if (pickResources_) {
        return true;
    }
    pickResources_ = std::make_unique<PickResources>();
    spdlog::info("[backend] pick resources created");
    return true;
}

void VulkanRenderer::destroyPickResources() {
    pickResources_.reset();
}

} // namespace moleculens::engine
