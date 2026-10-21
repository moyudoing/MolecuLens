// =============================================================================
// MolecuLens · engine · vulkan/backend/decode.cpp —— 命令流解码实现
// =============================================================================

#include "engine/vulkan/backend/decode.h"

#include <cstring>

#include "engine/vulkan/backend/draw_list.h"

namespace moleculens::engine {

void commandStats(const SceneCommandList& list, CommandStats* out) {
    if (out == nullptr) {
        return;
    }
    *out = CommandStats{};
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint32_t size = it.payloadSize();
        switch (it.code()) {
            case Cmd::SphereInst:
                out->spheres += size / sizeof(SphereInst);
                break;
            case Cmd::CylinderInst:
                out->cylinders += size / sizeof(CylinderInst);
                break;
            case Cmd::LineInst:
                out->lines += size / sizeof(LineInst);
                break;
            case Cmd::TriMesh:
                ++out->meshes;
                break;
            default:
                break;
        }
    }
}

void decodeCommandList(const SceneCommandList& list, DrawList& out) {
    out.clear();
    // 默认材质：保证任何 mat 索引（含未经 MaterialBlock 的几何）都可安全索引
    out.materials.push_back(MaterialBlock{});

    uint32_t currentMat = 0;
    uint8_t currentMode = 0;

    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint8_t* payload = it.payload();
        const uint32_t size = it.payloadSize();
        switch (it.code()) {
            case Cmd::MaterialBlock: {
                if (size >= sizeof(MaterialBlock)) {
                    MaterialBlock mat{};
                    std::memcpy(&mat, payload, sizeof(MaterialBlock));
                    // **去重**：内容与已有槽相同则复用下标。
                    // 为什么关键：绘制按 (材质下标, 模式) 的连续段分批
                    // （computeDrawRuns，见 draw_list.h）。发射端是**每实例**一条
                    // MaterialBlock（Representer::generate 按 (表示×实例) 调用），
                    // 若每条都占一个新槽，则段在每个实例边界断开 —— 实测 md.gro
                    // 单帧 ~4084 次绘制、每次平均仅 3.2 个实例。去重后同表示的实例
                    // 共用一个下标，段自然合并为"每 (表示, 模式) 一次"。
                    // 逐字段比较（不用 memcmp）：结构体虽无填充，但逐字段更明确，
                    // 也不会被未来新增字段时的填充字节影响。
                    uint32_t idx = UINT32_MAX;
                    for (uint32_t k = 0; k < out.materials.size(); ++k) {
                        const MaterialBlock& e = out.materials[k];
                        if (e.ambient == mat.ambient && e.diffuse == mat.diffuse &&
                            e.specular == mat.specular && e.shininess == mat.shininess &&
                            e.metallic == mat.metallic && e.roughness == mat.roughness &&
                            e.opacity == mat.opacity) {
                            idx = k;
                            break;
                        }
                    }
                    if (idx == UINT32_MAX) {
                        out.materials.push_back(mat);
                        idx = static_cast<uint32_t>(out.materials.size()) - 1;
                    }
                    currentMat = idx;
                }
                break;
            }
            case Cmd::RenderMode: {
                if (size >= sizeof(RenderModeInst)) {
                    RenderModeInst rmi{};
                    std::memcpy(&rmi, payload, sizeof(RenderModeInst));
                    currentMode = rmi.mode;
                }
                break;
            }
            case Cmd::SphereInst: {
                if (size >= sizeof(SphereInst) && size % sizeof(SphereInst) == 0) {
                    const uint32_t n = size / sizeof(SphereInst);
                    out.spheres.reserve(out.spheres.size() + n);
                    for (uint32_t i = 0; i < n; ++i) {
                        SphereInst inst{};
                        std::memcpy(&inst, payload + i * sizeof(SphereInst),
                                    sizeof(SphereInst));
                        out.spheres.push_back(inst);
                        out.sphereMat.push_back(currentMat);
                        out.sphereMode.push_back(currentMode);
                    }
                }
                break;
            }
            case Cmd::CylinderInst: {
                if (size >= sizeof(CylinderInst) && size % sizeof(CylinderInst) == 0) {
                    const uint32_t n = size / sizeof(CylinderInst);
                    out.cylinders.reserve(out.cylinders.size() + n);
                    for (uint32_t i = 0; i < n; ++i) {
                        CylinderInst inst{};
                        std::memcpy(&inst, payload + i * sizeof(CylinderInst),
                                    sizeof(CylinderInst));
                        out.cylinders.push_back(inst);
                        out.cylinderMat.push_back(currentMat);
                        out.cylinderMode.push_back(currentMode);
                    }
                }
                break;
            }
            case Cmd::LineInst: {
                if (size >= sizeof(LineInst) && size % sizeof(LineInst) == 0) {
                    const uint32_t n = size / sizeof(LineInst);
                    out.lines.reserve(out.lines.size() + n);
                    for (uint32_t i = 0; i < n; ++i) {
                        LineInst inst{};
                        std::memcpy(&inst, payload + i * sizeof(LineInst),
                                    sizeof(LineInst));
                        out.lines.push_back(inst);
                        out.lineMat.push_back(currentMat);
                    }
                }
                break;
            }
            case Cmd::TriMesh: {
                if (size >= sizeof(TriMeshHeader)) {
                    TriMeshHeader hdr{};
                    std::memcpy(&hdr, payload, sizeof(TriMeshHeader));
                    const uint32_t vertBytes = hdr.vertexCount * sizeof(TriMeshVertex);
                    const uint32_t idxBytes = hdr.indexCount * sizeof(uint32_t);
                    if (sizeof(TriMeshHeader) + vertBytes + idxBytes <= size) {
                        DrawList::Mesh mesh;
                        mesh.mat = currentMat;
                        mesh.mode = currentMode;
                        mesh.verts.resize(hdr.vertexCount);
                        mesh.idx.resize(hdr.indexCount);
                        std::memcpy(mesh.verts.data(), payload + sizeof(TriMeshHeader),
                                    vertBytes);
                        std::memcpy(mesh.idx.data(),
                                    payload + sizeof(TriMeshHeader) + vertBytes, idxBytes);
                        out.meshes.push_back(std::move(mesh));
                    }
                }
                break;
            }
            // 变换栈 / 裁剪面 / 渲染状态：当前无后端消费（主方案 §6.4 记为
            // 渲染状态，待接入）。显式列出以免 default 掩盖新增命令类型。
            case Cmd::Transform:
            case Cmd::PushMatrix:
            case Cmd::PopMatrix:
            case Cmd::ClipPlane:
            case Cmd::SetBlend:
            case Cmd::SetDepthWrite:
            // 已移除的 Label 表示，保留协议槽位：旧 .mlscene 命令流可能仍含该命令，
            // 解码时显式忽略（不参与绘制），保证旧文件可读。
            case Cmd::Label:
            case Cmd::Count:
                break;
        }
    }
}

} // namespace moleculens::engine
