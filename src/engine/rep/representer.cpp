// =============================================================================
// MolecuLens · engine · rep/representer.cpp —— 表示生成器实现
// 规格来源：主方案 §5.2 / §5.8.2（kDrawTable 分派；配色方案；材质块发射）
// =============================================================================

#include "engine/rep/representer.h"

#include <algorithm>
#include <cmath>

#include "engine/render/render_parameter.h"
#include "engine/style/quicksurf/quicksurf.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

// —— 风格生成器实现（各风格 cpp；统一签名 DrawFn）——
void drawSpacefill(const RepContext&, SceneCommandList&);
void drawBallAndStick(const RepContext&, SceneCommandList&);
void drawStick(const RepContext&, SceneCommandList&);
void drawWireframe(const RepContext&, SceneCommandList&);
void drawCartoon(const RepContext&, SceneCommandList&);
void drawSurface(const RepContext&, SceneCommandList&);
void drawQuickSurf(const RepContext&, SceneCommandList&);

// 预留风格（M2 后落地）：label
void drawLabelPlaceholder(const RepContext&, SceneCommandList&);

const Representer::DrawFn Representer::kDrawTable[
    static_cast<int>(RepresentationType::Count)] = {
    drawWireframe,        // Wireframe
    drawStick,            // Stick
    drawBallAndStick,     // BallAndStick
    drawSpacefill,        // Spacefill
    drawCartoon,          // Cartoon
    drawSurface,          // Surface（vdW 表面，Marching Cubes）
    drawLabelPlaceholder,    // Label（M2）
    drawQuickSurf,        // QuickSurf（高斯密度等值面）
};

namespace {

// HSL → RGB（链/残基配色）
glm::vec3 hslToRgb(float h, float s, float l) {
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    glm::vec3 rgb;
    if (hp < 1.0f) rgb = {c, x, 0};
    else if (hp < 2.0f) rgb = {x, c, 0};
    else if (hp < 3.0f) rgb = {0, c, x};
    else if (hp < 4.0f) rgb = {0, x, c};
    else if (hp < 5.0f) rgb = {x, 0, c};
    else rgb = {c, 0, x};
    const float m = l - c * 0.5f;
    return rgb + glm::vec3(m);
}

// 稳定哈希（FNV-1a）
uint32_t hash32(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

// BFactor 配色：蓝(0) → 白(0.5) → 红(1)（主方案 §5.2）
glm::vec3 bfactorColor(float b /* 0..1 */) {
    b = std::clamp(b, 0.0f, 1.0f);
    if (b < 0.5f) {
        const float t = b / 0.5f;
        return glm::mix(glm::vec3(0.16f, 0.32f, 0.97f), glm::vec3(1.0f), t);
    }
    const float t = (b - 0.5f) / 0.5f;
    return glm::mix(glm::vec3(1.0f), glm::vec3(0.97f, 0.16f, 0.16f), t);
}

} // namespace

const glm::vec3* Representer::globalPositions(const RepContext& ctx) {
    return ctx.frame && ctx.frame->positions ? ctx.frame->positions->data() : nullptr;
}

size_t Representer::globalAtomCount(const RepContext& ctx) {
    return ctx.frame && ctx.frame->positions ? ctx.frame->positions->size() : 0;
}

glm::vec4 Representer::colorForAtom(const RepContext& ctx, uint32_t globalAtomIndex) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const size_t localIdx = globalAtomIndex - ctx.globalAtomBase;
    if (localIdx >= st.elements.size()) {
        return glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    }
    switch (ctx.rep->colorScheme) {
        case ColorScheme::ElementCPK: {
            const auto& info = elementInfo(st.elements[localIdx]);
            return glm::vec4(info.color[0], info.color[1], info.color[2], 1.0f);
        }
        case ColorScheme::Chain: {
            const int32_t cid = st.atoms[localIdx].chainId;
            const float h = static_cast<float>(hash32(static_cast<uint32_t>(cid + 1)) %
                                               3600) / 3600.0f;
            const glm::vec3 rgb = hslToRgb(h, 0.65f, 0.55f);
            return glm::vec4(rgb, 1.0f);
        }
        case ColorScheme::Residue: {
            const int32_t rid = st.atoms[localIdx].residueId;
            const float h = static_cast<float>(hash32(static_cast<uint32_t>(rid + 1)) %
                                               3600) / 3600.0f;
            const glm::vec3 rgb = hslToRgb(h, 0.35f, 0.62f);
            return glm::vec4(rgb, 1.0f);
        }
        case ColorScheme::BFactor: {
            const float b = std::clamp(static_cast<float>(st.atoms[localIdx].bfactor) /
                                           20000.0f, 0.0f, 1.0f);
            return glm::vec4(bfactorColor(b), 1.0f);
        }
        case ColorScheme::Uniform:
        default:
            return glm::vec4(ctx.rep->diffuse[0], ctx.rep->diffuse[1],
                             ctx.rep->diffuse[2], ctx.rep->opacity);
    }
}

void Representer::generate(const Representation& rep,
                           const entity::SceneStructure& scene,
                           const entity::MoleculeInstance& mi,
                           uint32_t instanceIndex,
                           const entity::FrameData& frame,
                           uint64_t frameIndex,
                           const std::vector<bool>& mask,
                           SceneCommandList& out) {
    RepContext ctx;
    ctx.scene = &scene;
    ctx.instance = &mi;
    ctx.frame = &frame;
    ctx.rep = &rep;
    ctx.mask = &mask;
    ctx.instanceIndex = instanceIndex;
    ctx.globalAtomBase = scene.globalAtomBase(instanceIndex);
    ctx.frameIndex = frameIndex;
    // 体系渲染模式（每体系独立；后端据此切换 VkPipeline）
    ctx.renderMode = 0;
    if (mi.systemId != UINT32_MAX) {
        const uint32_t si = scene.systemIndexById(mi.systemId);
        if (si != UINT32_MAX) {
            ctx.renderMode = static_cast<uint8_t>(scene.systems()[si].renderMode);
        }
    }

    // 体系渲染模式命令：后端解码后切换到对应管线（实心/线框/透明/点）
    RenderModeInst rmi;
    rmi.mode = ctx.renderMode;
    rmi.pad[0] = rmi.pad[1] = rmi.pad[2] = 0;
    out.append(Cmd::RenderMode, &rmi, sizeof(rmi));

    // 材质状态（每表示一个材质，主方案 §6.6.3）
    MaterialBlock mat;
    mat.ambient = glm::vec4(rep.ambient[0], rep.ambient[1], rep.ambient[2], rep.ambient[3]);
    mat.diffuse = glm::vec4(rep.diffuse[0], rep.diffuse[1], rep.diffuse[2], rep.diffuse[3]);
    mat.specular = glm::vec4(rep.specular[0], rep.specular[1], rep.specular[2],
                             rep.specular[3]);
    mat.shininess = rep.shininess;
    mat.metallic = rep.metallic;
    mat.roughness = rep.roughness;
    mat.opacity = rep.opacity;
    if (ctx.renderMode == 2) {   // 透明模式：降低材质透明度（配合混合管线）
        mat.opacity = std::min(mat.opacity, 0.5f);
    }
    out.append(Cmd::MaterialBlock, &mat, sizeof(mat));

    // 分派
    const int idx = static_cast<int>(rep.type);
    if (idx >= 0 && idx < static_cast<int>(RepresentationType::Count)) {
        kDrawTable[idx](ctx, out);
    }
}

// —— 预留风格占位（M2 落地）——
void drawLabelPlaceholder(const RepContext&, SceneCommandList&) {}

} // namespace moleculens::engine
