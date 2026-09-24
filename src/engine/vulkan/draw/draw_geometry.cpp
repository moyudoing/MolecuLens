// =============================================================================
// Moleculens · engine · vulkan/draw/draw_geometry.cpp —— 共享几何绘制实现
// =============================================================================

#include "engine/vulkan/draw/draw_geometry.h"

#include <algorithm>
#include <vector>

#include <glm/glm.hpp>

#include "engine/vulkan/device/device.h"
#include "engine/vulkan/geometry/geometry.h"

namespace moleculens::engine {

namespace {

// 按 (体系渲染模式, 材质) 连续批次绘制实例化几何。
// pipes[4] 对应 DrawMode：0 实心 / 1 线框 / 2 透明 / 3 点。
// 点模式走 GL_POINTS（1 顶点/实例）；其余走网格索引 + 实例化。
template <typename InstT>
void drawModeRuns(VkCommandBuffer cmd, VulkanDevice& device,
                  const std::vector<InstT>& instances,
                  const std::vector<uint32_t>& mats,
                  const std::vector<uint8_t>& modes,
                  const std::vector<MaterialBlock>& materials,
                  Buffer& instanceBuffer, const Buffer& mesh, const Buffer& meshIdx,
                  uint32_t indexCount, const Pipeline* pipes[4], VkDescriptorSet set) {
    if (instances.empty()) {
        return;
    }
    const VkDeviceSize need = instances.size() * sizeof(InstT);
    if (!instanceBuffer.valid() || instanceBuffer.size() < need) {
        instanceBuffer.create(device, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    }
    instanceBuffer.upload(device, instances.data(), need);

    const auto drawRun = [&](uint32_t start, uint32_t count, uint32_t mode,
                             uint32_t matIdx) {
        const Pipeline& p = *pipes[mode % 4];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0, 1, &set,
                                0, nullptr);
        const MaterialBlock& mat = materials[matIdx];
        vkCmdPushConstants(cmd, p.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(MaterialBlock), &mat);
        if (mode == static_cast<uint32_t>(DrawMode::Point)) {
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

    uint32_t runStart = 0;
    uint32_t runMat = mats.empty() ? 0 : mats[0];
    uint8_t runMode = modes.empty() ? 0 : modes[0];
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
    drawRun(runStart, static_cast<uint32_t>(instances.size() - runStart), runMode, runMat);
}

// 上传线段顶点并绘制。顶点布局 = pos(float4) + color(float4) 交错 32B（line.vert）。
//
// ⚠ line.frag 直接输出顶点色、**不使用材质**（outColor = vColor），故此处不得把
//   材质乘进顶点色——否则线框会被额外压暗（实时后端曾如此，与导出图不一致）。
void drawLineVerts(VkCommandBuffer cmd, VulkanDevice& device, Buffer& lineVbo,
                   const std::vector<glm::vec4>& verts, const PipelineSet& pipelines,
                   const DrawList& draw, VkDescriptorSet set) {
    if (verts.empty()) {
        return;
    }
    const VkDeviceSize need = verts.size() * sizeof(glm::vec4);
    if (!lineVbo.valid() || lineVbo.size() < need) {
        lineVbo.create(device, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    }
    lineVbo.upload(device, verts.data(), need);

    const Pipeline& lp = pipelines.line();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lp.pipeline());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lp.layout(), 0, 1, &set, 0,
                            nullptr);
    // 材质在 line.frag 中未被使用；push 默认材质仅为满足管线布局的一致性
    const MaterialBlock mat = draw.materials.empty() ? MaterialBlock{} : draw.materials[0];
    vkCmdPushConstants(cmd, lp.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(MaterialBlock), &mat);
    VkBuffer vb = lineVbo.buffer();
    const VkDeviceSize offset = 0;
    // pOffsets 必须是非 NULL 有效指针（传 nullptr 为校验错误，曾致驱动崩溃 SIGSEGV）
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(verts.size() / 2), 1, 0, 0);
}

// 网格材质（diffuse/opacity）烘焙进顶点色：mesh.frag 用 mat.matDiffuse*vColor，
// 若硬编码 materials[0]（默认全 0）网格将全黑不可见（表面/卡通历史根因）。
inline void bakeMaterialIntoColor(TriMeshVertex& v, const MaterialBlock& mm) {
    // 逐分量乘（避免 GLM swizzle；本工程未开 GLM_FORCE_SWIZZLE）
    v.color = glm::vec4(v.color.r * mm.diffuse.x, v.color.g * mm.diffuse.y,
                        v.color.b * mm.diffuse.z, v.color.a * mm.opacity);
}

} // namespace

// —— 共享网格 ——

bool SharedMeshes::create(VulkanDevice& device) {
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> indices;

    generateIcosphere(verts, indices);   // 细分 2 级 → 320 三角形（主方案 §5.7.1）
    icosphereVertices = static_cast<uint32_t>(verts.size());
    icosphereIndices = static_cast<uint32_t>(indices.size());
    icosphere.create(device, verts.size() * sizeof(glm::vec3),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    icosphere.upload(device, verts.data(), verts.size() * sizeof(glm::vec3));
    icosphereIdx.create(device, indices.size() * sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    icosphereIdx.upload(device, indices.data(), indices.size() * sizeof(uint32_t));

    verts.clear();
    indices.clear();
    generateCylinder(verts, indices);    // 8 段无端盖（主方案 §5.7.3）
    cylinderVertices = static_cast<uint32_t>(verts.size());
    cylinderIndices = static_cast<uint32_t>(indices.size());
    cylinder.create(device, verts.size() * sizeof(glm::vec3),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    cylinder.upload(device, verts.data(), verts.size() * sizeof(glm::vec3));
    cylinderIdx.create(device, indices.size() * sizeof(uint32_t),
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    cylinderIdx.upload(device, indices.data(), indices.size() * sizeof(uint32_t));
    return true;
}

void SharedMeshes::destroy() {
    icosphere.destroy();
    icosphereIdx.destroy();
    cylinder.destroy();
    cylinderIdx.destroy();
    icosphereVertices = icosphereIndices = 0;
    cylinderVertices = cylinderIndices = 0;
}

void FrameGpuBuffers::destroy() {
    sphereInst.destroy();
    cylinderInst.destroy();
    lineVerts.destroy();
    meshVerts.destroy();
    meshIdx.destroy();
}

// —— 绘制入口 ——

void drawGeometry(VkCommandBuffer cmd, VulkanDevice& device, const PipelineSet& pipelines,
                  const DrawList& draw, FrameGpuBuffers& gpu, const SharedMeshes& meshes,
                  VkDescriptorSet globalSet) {
    // 体系渲染模式 → VkPipeline（实心 / 线框 / 透明 / 点）
    const Pipeline* spherePipes[4] = {
        &pipelines.sphere(DrawMode::Solid), &pipelines.sphere(DrawMode::Wireframe),
        &pipelines.sphere(DrawMode::Transparent), &pipelines.sphere(DrawMode::Point)};
    const Pipeline* cylPipes[4] = {
        &pipelines.cylinder(DrawMode::Solid), &pipelines.cylinder(DrawMode::Wireframe),
        &pipelines.cylinder(DrawMode::Transparent), &pipelines.cylinder(DrawMode::Point)};

    // —— 原子球（实例化，按 (模式,材质) 分批）——
    if (!draw.spheres.empty()) {
        drawModeRuns(cmd, device, draw.spheres, draw.sphereMat, draw.sphereMode,
                     draw.materials, gpu.sphereInst, meshes.icosphere, meshes.icosphereIdx,
                     meshes.icosphereIndices, spherePipes, globalSet);
    }

    // —— 化学键圆柱 ——
    if (!draw.cylinders.empty()) {
        // 点模式（mode 3）的键不走圆柱管线，而是拆成线段走 line 管线
        std::vector<CylinderInst> cylNonPoint;
        std::vector<uint32_t> cylMatNonPoint;
        std::vector<uint8_t> cylModeNonPoint;
        std::vector<glm::vec4> pointLineVerts;   // a, color, b, color
        for (size_t ci = 0; ci < draw.cylinders.size(); ++ci) {
            if (draw.cylinderMode[ci] == static_cast<uint8_t>(DrawMode::Point)) {
                const CylinderInst& c = draw.cylinders[ci];
                pointLineVerts.push_back(c.aPos);
                pointLineVerts.push_back(c.color);
                pointLineVerts.push_back(c.bPos);
                pointLineVerts.push_back(c.color);
            } else {
                cylNonPoint.push_back(draw.cylinders[ci]);
                cylMatNonPoint.push_back(draw.cylinderMat[ci]);
                cylModeNonPoint.push_back(draw.cylinderMode[ci]);
            }
        }
        if (!cylNonPoint.empty()) {
            drawModeRuns(cmd, device, cylNonPoint, cylMatNonPoint, cylModeNonPoint,
                         draw.materials, gpu.cylinderInst, meshes.cylinder,
                         meshes.cylinderIdx, meshes.cylinderIndices, cylPipes, globalSet);
        }
        drawLineVerts(cmd, device, gpu.lineVerts, pointLineVerts, pipelines, draw,
                      globalSet);
    }

    // —— 线段（线框 / 坐标轴 / 高亮）——
    if (!draw.lines.empty()) {
        std::vector<glm::vec4> lineVerts;
        lineVerts.reserve(draw.lines.size() * 4);
        for (const auto& l : draw.lines) {
            lineVerts.push_back(l.aPos);
            lineVerts.push_back(l.color);
            lineVerts.push_back(l.bPos);
            lineVerts.push_back(l.color);
        }
        drawLineVerts(cmd, device, gpu.lineVerts, lineVerts, pipelines, draw, globalSet);
    }

    // —— 三角网格（卡通 / 表面 / QuickSurf / 导入网格）——
    // 按体系渲染模式分组合并绘制（点模式网格按实心降级，见 PipelineSet::mesh）。
    if (!draw.meshes.empty()) {
        for (int mode = 0; mode < static_cast<int>(DrawMode::Count); ++mode) {
            size_t totalVerts = 0, totalIdx = 0;
            for (const auto& mesh : draw.meshes) {
                if (mesh.mode == mode) {
                    totalVerts += mesh.verts.size();
                    totalIdx += mesh.idx.size();
                }
            }
            if (totalVerts == 0 || totalIdx == 0) {
                continue;
            }

            std::vector<TriMeshVertex> allVerts;
            std::vector<uint32_t> allIdx;
            allVerts.reserve(totalVerts);
            allIdx.reserve(totalIdx);
            for (const auto& mesh : draw.meshes) {
                if (mesh.mode != mode) {
                    continue;
                }
                const uint32_t base = static_cast<uint32_t>(allVerts.size());
                const MaterialBlock& mm =
                    draw.materials[mesh.mat < draw.materials.size() ? mesh.mat : 0];
                for (const auto& v : mesh.verts) {
                    TriMeshVertex tv = v;
                    bakeMaterialIntoColor(tv, mm);
                    allVerts.push_back(tv);
                }
                for (uint32_t i : mesh.idx) {
                    allIdx.push_back(base + i);
                }
            }

            const VkDeviceSize vertNeed = allVerts.size() * sizeof(TriMeshVertex);
            if (!gpu.meshVerts.valid() || gpu.meshVerts.size() < vertNeed) {
                gpu.meshVerts.create(device, vertNeed, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            gpu.meshVerts.upload(device, allVerts.data(), vertNeed);
            const VkDeviceSize idxNeed = allIdx.size() * sizeof(uint32_t);
            if (!gpu.meshIdx.valid() || gpu.meshIdx.size() < idxNeed) {
                gpu.meshIdx.create(device, idxNeed, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            gpu.meshIdx.upload(device, allIdx.data(), idxNeed);

            const Pipeline& mp = pipelines.mesh(static_cast<DrawMode>(mode));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.layout(), 0, 1,
                                    &globalSet, 0, nullptr);
            // 漫反射已烘焙进顶点色 → push 白色默认材质（diffuse=1）避免二次变暗
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
            VkBuffer vb = gpu.meshVerts.buffer();
            const VkDeviceSize meshOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &meshOffset);
            vkCmdBindIndexBuffer(cmd, gpu.meshIdx.buffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, static_cast<uint32_t>(allIdx.size()), 1, 0, 0, 0);
        }
    }
}

} // namespace moleculens::engine
