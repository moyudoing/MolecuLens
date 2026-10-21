// =============================================================================
// Moleculens · engine · vulkan/draw/draw_geometry.cpp —— 共享几何绘制实现
// =============================================================================

#include "engine/vulkan/draw/draw_geometry.h"
#include "engine/render/renderer.h"   // RenderView（视锥剔除用相机矩阵）

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
                  uint32_t indexCount, const Pipeline* pipes[4], VkDescriptorSet set,
                  bool reupload, uint32_t& drawCallsOut) {
    if (instances.empty()) {
        return;
    }
    const VkDeviceSize need = instances.size() * sizeof(InstT);
    if (!instanceBuffer.valid() || instanceBuffer.size() < need) {
        instanceBuffer.create(device, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        reupload = true;   // 缓冲刚重建 → 内容为空，必须写入
    }
    if (reupload) {
        instanceBuffer.upload(device, instances.data(), need);
    }

    const auto drawRun = [&](uint32_t start, uint32_t count, uint32_t mode,
                             uint32_t matIdx) {
        // 管线按体系渲染模式选；但**材质不透明度 < 1 时必须走混合管线**。
        // 片元着色器已按 matOpacity 输出 alpha（sphere/cylinder/mesh.frag），
        // 而在非混合管线里 alpha 被直接丢弃 —— 材质面板的"不透明度"滑杆会毫无
        // 视觉效果（控件能用、观感不变，正是最难被发现的那类失效）。
        // 点/线框是用户对体系的显式选择，保持其管线不变。
        uint32_t pipeMode = mode % 4;
        const float matOpacity =
            matIdx < materials.size() ? materials[matIdx].opacity : 1.0f;
        if (pipeMode == static_cast<uint32_t>(DrawMode::Solid) && matOpacity < 0.999f) {
            pipeMode = static_cast<uint32_t>(DrawMode::Transparent);
        }
        const Pipeline& p = *pipes[pipeMode];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout(), 0, 1, &set,
                                0, nullptr);
        // push 一份本地副本：半透明几何要单独关掉"接收动态阴影"。
        // 不靠片元里的 alpha 判断——透明网格的 opacity 已烘焙进顶点 alpha，
        // 推送的 MaterialBlock.opacity 恒为 1，片元无法区分透明与不透明。
        MaterialBlock mat =
            matIdx < materials.size() ? materials[matIdx] : MaterialBlock{};
        const bool translucent =
            mat.opacity < 0.999f || pipeMode == static_cast<uint32_t>(DrawMode::Transparent);
        mat.shadowMask = translucent ? 0.0f : 1.0f;
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

    // 分段规则收敛到 computeDrawRuns（单一事实来源，见 draw_list.h）：
    // 它同时决定这里的提交次数与 commandStats/drawCalls 的计数，两者不会漂移。
    for (const DrawRun& r : computeDrawRuns(mats, modes, instances.size())) {
        drawRun(r.start, r.count, r.mode, r.mat);
        ++drawCallsOut;
    }
}

// 上传线段顶点并绘制。顶点布局 = pos(float4) + color(float4) 交错 32B（line.vert）。
//
// ⚠ line.frag 直接输出顶点色、**不使用材质**（outColor = vColor），故此处不得把
//   材质乘进顶点色——否则线框会被额外压暗（实时后端曾如此，与导出图不一致）。
// drawCallsOut：**在真正提交绘制之后**自增 —— 计数器必须在发出 vkCmdDraw 的那一处
// 更新，调用点"调完就加"会把空输入（本函数提前返回、一次都没提交）也算成一次绘制，
// 让"绘制调用数"这个指标虚高（实测把 2 次报成 3 次）。
void drawLineVerts(VkCommandBuffer cmd, VulkanDevice& device, Buffer& lineVbo,
                   const std::vector<glm::vec4>& verts, const PipelineSet& pipelines,
                   const DrawList& draw, VkDescriptorSet set, bool reupload,
                   uint32_t& drawCallsOut) {
    if (verts.empty()) {
        return;
    }
    const VkDeviceSize need = verts.size() * sizeof(glm::vec4);
    if (!lineVbo.valid() || lineVbo.size() < need) {
        lineVbo.create(device, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        reupload = true;
    }
    if (reupload) {
        lineVbo.upload(device, verts.data(), need);
    }

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
    ++drawCallsOut;
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

bool SharedMeshes::create(VulkanDevice& device, int detail, int segments) {
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> indices;

    generateIcosphere(verts, indices, detail);   // detail 档：1=80 / 2=320 / 3=1280 / 4=5120 面
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
    generateCylinder(verts, indices, segments);   // segments 段无端盖（预览 24 / 导出 48）
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
    uploadRevision = UINT64_MAX;   // 缓冲已销毁 → 下次必须完整重传
    for (auto& r : meshRanges) {
        r = MeshDrawRange{};
    }
}

// —— 绘制入口 ——

void drawGeometry(VkCommandBuffer cmd, VulkanDevice& device, const PipelineSet& pipelines,
                  const DrawList& draw, FrameGpuBuffers& gpu, const SharedMeshes& meshes,
                  VkDescriptorSet globalSet, uint64_t listRevision, const RenderView* view) {
    // 内容未变（相机移动 / UI 重绘等不改几何的帧）→ 复用上次上传的缓冲，
    // 不重传任何实例/顶点数据。缓冲仍在（shutdown 才销毁），内容逐字节一致。
    bool reupload = gpu.uploadRevision != listRevision;
    // 本帧实际提交的绘制调用数（ground truth，供性能观测：批处理是否生效）
    gpu.drawCalls = 0;

    // 体系渲染模式 → VkPipeline（实心 / 线框 / 透明 / 点）
    const Pipeline* spherePipes[4] = {
        &pipelines.sphere(DrawMode::Solid), &pipelines.sphere(DrawMode::Wireframe),
        &pipelines.sphere(DrawMode::Transparent), &pipelines.sphere(DrawMode::Point)};
    const Pipeline* cylPipes[4] = {
        &pipelines.cylinder(DrawMode::Solid), &pipelines.cylinder(DrawMode::Wireframe),
        &pipelines.cylinder(DrawMode::Transparent), &pipelines.cylinder(DrawMode::Point)};

    // —— 视锥剔除（P0）：相机存在时，用 6 个视锥平面过滤不可见的球/柱实例 ——
    struct FrustumPlanes {
        glm::vec4 p[6];
        bool valid = false;
        void fromViewProj(const glm::mat4& m) {
            // GLM 列主序：第 i 行 = (m[0][i], m[1][i], m[2][i], m[3][i])
            p[0] = glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);
            p[1] = glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);
            p[2] = glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);
            p[3] = glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);
            p[4] = glm::vec4(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2]);
            p[5] = glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);
            valid = true;
        }
        [[nodiscard]] bool test(const glm::vec3& c, float r) const {
            if (!valid) return true;
            for (int i = 0; i < 6; ++i) {
                if (p[i].x * c.x + p[i].y * c.y + p[i].z * c.z + p[i].w < -r) return false;
            }
            return true;
        }
        [[nodiscard]] bool testSegment(const glm::vec3& a, const glm::vec3& b, float r) const {
            if (!valid) return true;
            const glm::vec3 mid = (a + b) * 0.5f;
            const float half = glm::length(b - a) * 0.5f;
            return test(mid, half + r);
        }
    } frustum;

    const bool cull = view != nullptr && view->height > 0;
    if (cull) {
        const float aspect = static_cast<float>(view->width) / static_cast<float>(view->height);
        frustum.fromViewProj(view->camera.viewProjMatrix(aspect));
    }
    // 剔除后保留下来的实例（未启用剔除时直接指向原列表，零拷贝）
    const std::vector<SphereInst>* sphereList = &draw.spheres;
    const std::vector<uint32_t>* sphereMatList = &draw.sphereMat;
    const std::vector<uint8_t>* sphereModeList = &draw.sphereMode;
    std::vector<SphereInst> sphereKept;
    std::vector<uint32_t> sphereMatKept;
    std::vector<uint8_t> sphereModeKept;
    if (cull && !draw.spheres.empty()) {
        sphereKept.reserve(draw.spheres.size());
        for (size_t i = 0; i < draw.spheres.size(); ++i) {
            const SphereInst& s = draw.spheres[i];
            if (!frustum.test(glm::vec3(s.posRadius), s.posRadius.w)) continue;
            sphereKept.push_back(s);
            if (i < draw.sphereMat.size()) sphereMatKept.push_back(draw.sphereMat[i]);
            if (i < draw.sphereMode.size()) sphereModeKept.push_back(draw.sphereMode[i]);
        }
        sphereList = &sphereKept;
        sphereMatList = &sphereMatKept;
        sphereModeList = &sphereModeKept;
    }

    // —— 原子球（实例化，按 (模式,材质) 分批）——
    if (!sphereList->empty()) {
        drawModeRuns(cmd, device, *sphereList, *sphereMatList, *sphereModeList,
                     draw.materials, gpu.sphereInst, meshes.icosphere, meshes.icosphereIdx,
                     meshes.icosphereIndices, spherePipes, globalSet, reupload,
                     gpu.drawCalls);
    }

    // —— 化学键圆柱 ——
    if (!draw.cylinders.empty()) {
        // 点模式（mode 3）的键不走圆柱管线，而是拆成线段走 line 管线
        std::vector<CylinderInst> cylNonPoint;
        std::vector<uint32_t> cylMatNonPoint;
        std::vector<uint8_t> cylModeNonPoint;
        std::vector<glm::vec4> pointLineVerts;   // a, color, b, color
        for (size_t ci = 0; ci < draw.cylinders.size(); ++ci) {
            // 视锥剔除：线段两端点 + 半径构成包围球
            if (cull) {
                const CylinderInst& cc = draw.cylinders[ci];
                if (!frustum.testSegment(glm::vec3(cc.aPos), glm::vec3(cc.bPos), cc.radius)) {
                    continue;
                }
            }
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
                         meshes.cylinderIdx, meshes.cylinderIndices, cylPipes, globalSet,
                         reupload, gpu.drawCalls);
        }
        drawLineVerts(cmd, device, gpu.lineVerts, pointLineVerts, pipelines, draw,
                      globalSet, reupload, gpu.drawCalls);
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
        drawLineVerts(cmd, device, gpu.lineVerts, lineVerts, pipelines, draw, globalSet,
                      reupload, gpu.drawCalls);
    }

    // —— 三角网格（卡通 / 表面 / QuickSurf / 导入网格）——
    // 合并进一块顶点/索引缓冲，按渲染模式分段提交（组内若有半透明材质则该组走
    // 混合管线，原因见下方注释）。
    // 性能：合并 + 材质烘焙只在命令流修订号变化时执行一次；未变帧直接复用
    // 缓存的 meshRanges（此前无论内容是否变化，每帧都会把全部网格顶点复制并
    // 重新烘焙一遍 —— QuickSurf 表面几十万顶点时静止帧也要白白复制十几 MB）。
    if (reupload) {
        for (auto& r : gpu.meshRanges) {
            r = FrameGpuBuffers::MeshDrawRange{};
        }
        std::vector<TriMeshVertex> allVerts;
        std::vector<uint32_t> allIdx;
        size_t totalVerts = 0, totalIdx = 0;
        for (const auto& mesh : draw.meshes) {
            totalVerts += mesh.verts.size();
            totalIdx += mesh.idx.size();
        }
        allVerts.reserve(totalVerts);
        allIdx.reserve(totalIdx);

        for (int mode = 0; mode < static_cast<int>(DrawMode::Count); ++mode) {
            const uint32_t firstIndex = static_cast<uint32_t>(allIdx.size());
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
            const uint32_t count = static_cast<uint32_t>(allIdx.size()) - firstIndex;
            if (count == 0) {
                continue;
            }
            // 组内任一网格的材质不透明度 < 1 → 走混合管线。
            // 表面/卡通常以半透明呈现（"表面 + 内部球棍"是分子可视化的常见组合），
            // 而 mesh.frag 也是按 matOpacity 输出 alpha 的，不换管线就完全看不到透明。
            int pipeMode = mode;
            if (pipeMode == static_cast<int>(DrawMode::Solid)) {
                for (const auto& mesh : draw.meshes) {
                    if (mesh.mode != mode) {
                        continue;
                    }
                    const MaterialBlock& mm =
                        draw.materials[mesh.mat < draw.materials.size() ? mesh.mat : 0];
                    if (mm.opacity < 0.999f) {
                        pipeMode = static_cast<int>(DrawMode::Transparent);
                        break;
                    }
                }
            }
            gpu.meshRanges[mode].firstIndex = firstIndex;
            gpu.meshRanges[mode].indexCount = count;
            gpu.meshRanges[mode].pipeMode = static_cast<uint8_t>(pipeMode);
            // 记录本段第一个网格的材质下标（透明段会用它自己的 ambient/specular/
            // shininess，见下方绘制循环）。空段不关心。
            for (const auto& mesh : draw.meshes) {
                if (mesh.mode == mode) {
                    gpu.meshRanges[mode].materialIndex = mesh.mat;
                    break;
                }
            }
        }

        const VkDeviceSize vertNeed = allVerts.size() * sizeof(TriMeshVertex);
        const VkDeviceSize idxNeed = allIdx.size() * sizeof(uint32_t);
        if (vertNeed > 0 && idxNeed > 0) {
            if (!gpu.meshVerts.valid() || gpu.meshVerts.size() < vertNeed) {
                gpu.meshVerts.create(device, vertNeed, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            if (!gpu.meshIdx.valid() || gpu.meshIdx.size() < idxNeed) {
                gpu.meshIdx.create(device, idxNeed, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
            }
            gpu.meshVerts.upload(device, allVerts.data(), vertNeed);
            gpu.meshIdx.upload(device, allIdx.data(), idxNeed);
        }
    }

    if (gpu.meshVerts.valid() && gpu.meshIdx.valid()) {
        for (int mode = 0; mode < static_cast<int>(DrawMode::Count); ++mode) {
            const auto& range = gpu.meshRanges[mode];
            if (range.indexCount == 0) {
                continue;
            }
            const Pipeline& mp = pipelines.mesh(static_cast<DrawMode>(range.pipeMode));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.pipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mp.layout(), 0, 1,
                                    &globalSet, 0, nullptr);
            // 漫反射已烘焙进顶点色 → push 材质时 diffuse 固定为白，避免二次变暗。
            // 透明段：使用网格自己的材质（ambient/specular/shininess/roughness），
            // 否则半透明表面（ESP/QuickSurf）的高光与环境色设置全部失效 ——
            // 会被固定默认材质刷亮、发灰（用户反馈「颜色渲染差太多」的根因之一）。
            MaterialBlock mat;
            mat.ambient = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f);
            mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            mat.specular = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);
            mat.shininess = 32.0f;
            mat.metallic = 0.0f;
            mat.roughness = 0.4f;
            mat.opacity = 1.0f;
            // 混合管线（半透明表面/卡通）不接收动态阴影：壳内球棍的影子会落在
            // 壳上，形成与分子同形的黑色幽灵（用户反馈的"表面光线透视问题"）。
            mat.shadowMask =
                range.pipeMode == static_cast<uint8_t>(DrawMode::Transparent) ? 0.0f : 1.0f;
            if (range.pipeMode == static_cast<uint8_t>(DrawMode::Transparent) &&
                !draw.materials.empty()) {
                const std::size_t mi = std::min<std::size_t>(
                    range.materialIndex, draw.materials.size() - 1);
                mat = draw.materials[mi];
                mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // 已烘焙
                mat.opacity = 1.0f;                              // 已烘焙进顶点 alpha
                mat.shadowMask = 0.0f;                           // 半透明 ⇒ 不接收阴影
            }
            vkCmdPushConstants(cmd, mp.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MaterialBlock), &mat);
            VkBuffer vb = gpu.meshVerts.buffer();
            const VkDeviceSize meshOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &meshOffset);
            vkCmdBindIndexBuffer(cmd, gpu.meshIdx.buffer(), 0, VK_INDEX_TYPE_UINT32);
            // 参数顺序：indexCount, instanceCount, firstIndex, vertexOffset, firstInstance
            //（旧实现把 range.firstIndex 传到了 firstInstance 位置 → 多渲染模式时
            //  索引段起点失效；单段（firstIndex=0）时碰巧正确，故长期未暴露。）
            vkCmdDrawIndexed(cmd, range.indexCount, 1, range.firstIndex, 0, 0);
            ++gpu.drawCalls;
        }
    }

    // 记录本次内容来源：下一帧同修订号即跳过上述全部上传
    gpu.uploadRevision = listRevision;
}

} // namespace moleculens::engine
