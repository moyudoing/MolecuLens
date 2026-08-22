// =============================================================================
// MolecuLens · engine · vulkan/offscreen/offscreen_renderer.cpp —— 离屏渲染实现
// 规格来源：主方案 §6.1/§12.5（无 Swapchain；MSAA resolve；像素回读 RGBA8/16F）
// =============================================================================

#include "engine/vulkan/offscreen/offscreen_renderer.h"

#include <algorithm>
#include <cstring>
#include <numeric>

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

glm::mat4 vkProjection(const glm::mat4& proj) {
    glm::mat4 m = proj;
    m[1][1] *= -1.0f;
    m[2][2] = m[2][2] * 0.5f + m[3][2] * 0.5f;
    m[2][3] = m[2][3] * 0.5f;
    return m;
}

} // namespace

namespace {
// 网格生成（与 realtime 共享实现；放匿名命名空间避免重复符号）
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

    if (!device_.initHeadless(config_, outError)) {
        return false;
    }
    if (!createFramebuffer()) {
        return false;
    }

    // 管线（与 realtime 相同布局；片元光照 + 材质 push constant）
    auto loadShader = [&](ShaderModule*& out, VkShaderStageFlagBits stage,
                          const uint8_t* spv, size_t size) -> bool {
        out = new ShaderModule();
        return out->create(device_, stage, spv, size);
    };
    ShaderModule* sphereVert = nullptr;
    ShaderModule* sphereFrag = nullptr;
    ShaderModule* cylinderVert = nullptr;
    ShaderModule* cylinderFrag = nullptr;
    ShaderModule* lineVert = nullptr;
    ShaderModule* lineFrag = nullptr;
    ShaderModule* meshVert = nullptr;
    ShaderModule* meshFrag = nullptr;
    if (!loadShader(sphereVert, VK_SHADER_STAGE_VERTEX_BIT, sphere_vert, sphere_vert_len) ||
        !loadShader(sphereFrag, VK_SHADER_STAGE_FRAGMENT_BIT, sphere_frag, sphere_frag_len) ||
        !loadShader(cylinderVert, VK_SHADER_STAGE_VERTEX_BIT, cylinder_vert, cylinder_vert_len) ||
        !loadShader(cylinderFrag, VK_SHADER_STAGE_FRAGMENT_BIT, cylinder_frag, cylinder_frag_len) ||
        !loadShader(lineVert, VK_SHADER_STAGE_VERTEX_BIT, line_vert, line_vert_len) ||
        !loadShader(lineFrag, VK_SHADER_STAGE_FRAGMENT_BIT, line_frag, line_frag_len) ||
        !loadShader(meshVert, VK_SHADER_STAGE_VERTEX_BIT, mesh_vert, mesh_vert_len) ||
        !loadShader(meshFrag, VK_SHADER_STAGE_FRAGMENT_BIT, mesh_frag, mesh_frag_len)) {
        delete sphereVert; delete sphereFrag; delete cylinderVert; delete cylinderFrag;
        delete lineVert; delete lineFrag; delete meshVert; delete meshFrag;
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (!globalLayout_.create(device_, {binding}) ||
        !globalUbo_.create(device_, globalLayout_.layout(), sizeof(GlobalUBO))) {
        return false;
    }

    const VkRenderPass rp = renderPass_.renderPass();
    const VkSampleCountFlagBits samples = device_.msaaSamples();

    // sphere
    {
        const VkPipelineShaderStageCreateInfo stages[] = {sphereVert->stage(), sphereFrag->stage()};
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
        PipelineBuilder pb;
        pb.renderPass(rp).samples(samples)
            .shaders(stages, 2)
            .vertexInput({bindings, bindings + 2}, {attrs, attrs + 5})
            .descriptorLayouts({globalLayout_.layout()})
            .pushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MaterialBlock))
            .depthTest(true).depthWrite(true).blend(false).cullMode(VK_CULL_MODE_NONE);
        pb.build(device_, &spherePipeline_);
    }
    // cylinder
    {
        const VkPipelineShaderStageCreateInfo stages[] = {cylinderVert->stage(), cylinderFrag->stage()};
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
        PipelineBuilder pb;
        pb.renderPass(rp).samples(samples)
            .shaders(stages, 2)
            .vertexInput({bindings, bindings + 2}, {attrs, attrs + 6})
            .descriptorLayouts({globalLayout_.layout()})
            .pushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MaterialBlock))
            .depthTest(true).depthWrite(true).blend(false).cullMode(VK_CULL_MODE_NONE);
        pb.build(device_, &cylinderPipeline_);
    }
    // line
    {
        const VkPipelineShaderStageCreateInfo stages[] = {lineVert->stage(), lineFrag->stage()};
        const VkVertexInputBindingDescription bindings[] = {
            {0, 48, VK_VERTEX_INPUT_RATE_VERTEX},
        };
        const VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        };
        PipelineBuilder pb;
        pb.renderPass(rp).samples(samples).topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
            .shaders(stages, 2)
            .vertexInput({bindings, bindings + 1}, {attrs, attrs + 2})
            .descriptorLayouts({globalLayout_.layout()})
            .pushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MaterialBlock))
            .depthTest(true).depthWrite(false).blend(true).lineWidth(1.0f)
            .cullMode(VK_CULL_MODE_NONE);
        pb.build(device_, &linePipeline_);
    }
    // mesh
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
        PipelineBuilder pb;
        pb.renderPass(rp).samples(samples)
            .shaders(stages, 2)
            .vertexInput({bindings, bindings + 1}, {attrs, attrs + 3})
            .descriptorLayouts({globalLayout_.layout()})
            .pushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MaterialBlock))
            .depthTest(true).depthWrite(true).blend(false).cullMode(VK_CULL_MODE_NONE);
        pb.build(device_, &meshPipeline_);
    }
    delete sphereVert; delete sphereFrag; delete cylinderVert; delete cylinderFrag;
    delete lineVert; delete lineFrag; delete meshVert; delete meshFrag;

    // 共享网格
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> indices;
    generateIcosphereImpl(verts, indices);
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
    verts.clear();
    indices.clear();
    generateCylinderImpl(verts, indices);
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

    drawBuffers_ = std::make_unique<DrawBuffers>();
    initialized_ = true;
    spdlog::info("[backend] OffscreenRenderer initialized ({}x{})", width_, height_);
    return true;
}

void OffscreenRenderer::shutdown() {
    if (!initialized_) {
        return;
    }
    vkDeviceWaitIdle(device_.device());
    drawBuffers_.reset();
    icosphere_.reset();
    cylinder_.reset();
    spherePipeline_.destroy(device_);
    cylinderPipeline_.destroy(device_);
    linePipeline_.destroy(device_);
    meshPipeline_.destroy(device_);
    globalUbo_.destroy();
    globalLayout_.destroy();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.device(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    destroyFramebuffer();
    if (readbackBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_.device(), readbackBuffer_, nullptr);
        vkFreeMemory(device_.device(), readbackMemory_, nullptr);
        readbackBuffer_ = VK_NULL_HANDLE;
        readbackMemory_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
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

    if (!colorImage_.create(device_, colorFormat, width_, height_, samples,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    if (!depthImage_.create(device_, depthFormat, width_, height_, samples,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        return false;
    }
    if (!renderPass_.createOffscreen(device_, colorFormat, samples,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
        return false;
    }
    if (!framebuffer_.create(device_, renderPass_.renderPass(),
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
        vkCreateSampler(device_.device(), &info, nullptr, &sampler_);
    }
    colorLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void OffscreenRenderer::destroyFramebuffer() {
    framebuffer_.destroy();
    renderPass_.destroy();
    depthImage_.destroy();
    colorImage_.destroy();
}

void OffscreenRenderer::resize(uint32_t width, uint32_t height) {
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    vkDeviceWaitIdle(device_.device());
    destroyFramebuffer();
    createFramebuffer();
}

// —— 渲染 ——

void OffscreenRenderer::execute(const SceneCommandList& list, const RenderView& view) {
    auto& buf = *drawBuffers_;
    buf.spheres.clear();
    buf.sphereMat.clear();
    buf.cylinders.clear();
    buf.cylinderMat.clear();
    buf.lines.clear();
    buf.lineMat.clear();
    buf.meshes.clear();
    buf.materials.clear();

    uint32_t currentMat = 0;
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
            case Cmd::SphereInst: {
                if (size >= sizeof(SphereInst) && size % sizeof(SphereInst) == 0) {
                    const uint32_t n = size / sizeof(SphereInst);
                    for (uint32_t i = 0; i < n; ++i) {
                        SphereInst inst;
                        std::memcpy(&inst, payload + i * sizeof(SphereInst),
                                    sizeof(SphereInst));
                        buf.spheres.push_back(inst);
                        buf.sphereMat.push_back(currentMat);
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
    ubo.viewProj = vkProjection(view.camera.viewProjMatrix(
        static_cast<float>(width_) / static_cast<float>(height_)));
    ubo.cameraPos = glm::vec4(view.camera.position(), 1.0f);
    ubo.lightDir = glm::vec4(0.3f, -0.8f, 0.5f, 0.0f);
    ubo.lightColor = glm::vec4(1.0f);
    ubo.ambient = glm::vec4(0.5f, 0.55f, 0.7f, 0.35f);
    ubo.fog = glm::vec4(0.0f, 0.0f, 0.0f, 0.05f);
    ubo.sceneProps = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    globalUbo_.update(0, ubo);

    VkCommandBuffer cmd = device_.beginSingleTimeCommands();
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
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkDescriptorSet globalSet = globalUbo_.set(0);
    if (!buf.spheres.empty()) {
        drawInstancedBatches(cmd, buf.spheres, buf.sphereMat, buf.materials,
                             buf.sphereInst, *icosphere_, *icosphereIdx_,
                             icosphereVerts_, icosphereIndices_,
                             spherePipeline_.pipeline(), spherePipeline_.layout(),
                             globalSet);
    }
    if (!buf.cylinders.empty()) {
        drawInstancedBatches(cmd, buf.cylinders, buf.cylinderMat, buf.materials,
                             buf.cylinderInst, *cylinder_, *cylinderIdx_,
                             cylinderVerts_, cylinderIndices_,
                             cylinderPipeline_.pipeline(), cylinderPipeline_.layout(),
                             globalSet);
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
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, nullptr);
        vkCmdDraw(cmd, static_cast<uint32_t>(lineVerts.size() / 2), 1, 0, 0);
    }
    if (!buf.meshes.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                meshPipeline_.layout(), 0, 1, &globalSet, 0, nullptr);
        size_t totalVerts = 0, totalIdx = 0;
        for (const auto& mesh : buf.meshes) {
            totalVerts += mesh.verts.size();
            totalIdx += mesh.idx.size();
        }
        std::vector<TriMeshVertex> allVerts;
        std::vector<uint32_t> allIdx;
        allVerts.reserve(totalVerts);
        allIdx.reserve(totalIdx);
        for (const auto& mesh : buf.meshes) {
            const uint32_t base = static_cast<uint32_t>(allVerts.size());
            allVerts.insert(allVerts.end(), mesh.verts.begin(), mesh.verts.end());
            for (uint32_t i : mesh.idx) {
                allIdx.push_back(base + i);
            }
        }
        if (!allVerts.empty() && !allIdx.empty()) {
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
            const MaterialBlock& mat = buf.materials[0];
            vkCmdPushConstants(cmd, meshPipeline_.layout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(MaterialBlock), &mat);
            VkBuffer vb = buf.meshVerts.buffer();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, nullptr);
            vkCmdBindIndexBuffer(cmd, buf.meshIdx.buffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, static_cast<uint32_t>(allIdx.size()), 1, 0, 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
    device_.endSingleTimeCommands(cmd);
}

template <typename InstT>
void OffscreenRenderer::drawInstancedBatches(
    VkCommandBuffer cmd, const std::vector<InstT>& instances,
    const std::vector<uint32_t>& mats,
    const std::vector<MaterialBlock>& materials, Buffer& instanceBuffer,
    const Buffer& mesh, const Buffer& meshIdx, uint32_t vertexCount,
    uint32_t indexCount, VkPipeline pipeline, VkPipelineLayout layout,
    VkDescriptorSet set) {
    if (instances.empty()) {
        return;
    }
    (void)vertexCount;   // 实例化绘制：顶点数由共享网格索引决定
    const VkDeviceSize need = instances.size() * sizeof(InstT);
    if (!instanceBuffer.valid() || instanceBuffer.size() < need) {
        instanceBuffer.create(device_, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    }
    instanceBuffer.upload(device_, instances.data(), need);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                            nullptr);
    VkBuffer vertexBufs[] = {mesh.buffer(), instanceBuffer.buffer()};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBufs, offsets);
    vkCmdBindIndexBuffer(cmd, meshIdx.buffer(), 0, VK_INDEX_TYPE_UINT32);

    // 按材质分批（每表示一个材质 → DrawCall 量 = 表示数，主方案 §6.6.3）
    uint32_t runStart = 0;
    uint32_t runMat = mats.empty() ? 0 : mats[0];
    const auto drawRun = [&](uint32_t start, uint32_t count, uint32_t matIdx) {
        const MaterialBlock& mat = materials[matIdx];
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(MaterialBlock), &mat);
        vkCmdDrawIndexed(cmd, indexCount, count, 0, 0, start);
    };
    for (size_t i = 1; i < instances.size(); ++i) {
        if (mats[i] != runMat) {
            drawRun(runStart, static_cast<uint32_t>(i - runStart), runMat);
            runStart = static_cast<uint32_t>(i);
            runMat = mats[i];
        }
    }
    drawRun(runStart, static_cast<uint32_t>(instances.size() - runStart), runMat);
}

bool OffscreenRenderer::readbackPixels(std::vector<uint8_t>& out, PixelFormat fmt) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ *
                               (fmt == PixelFormat::Rgba16F ? 8 : 4);
    VkCommandBuffer cmd = device_.beginSingleTimeCommands();

    // colorImage → readback buffer
    if (readbackBuffer_ == VK_NULL_HANDLE || readbackSize_ < bytes) {
        if (readbackBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_.device(), readbackBuffer_, nullptr);
            vkFreeMemory(device_.device(), readbackMemory_, nullptr);
        }
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device_.device(), &info, nullptr, &readbackBuffer_);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_.device(), readbackBuffer_, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(device_.physicalDevice(), &props);
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
        vkAllocateMemory(device_.device(), &alloc, nullptr, &readbackMemory_);
        vkBindBufferMemory(device_.device(), readbackBuffer_, readbackMemory_, 0);
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
    device_.endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    vkMapMemory(device_.device(), readbackMemory_, 0, bytes, 0, &mapped);
    if (!mapped) {
        return false;
    }
    out.assign(static_cast<const uint8_t*>(mapped),
               static_cast<const uint8_t*>(mapped) + bytes);
    pixels_ = out;
    vkUnmapMemory(device_.device(), readbackMemory_);
    return true;
}

} // namespace moleculens::engine
