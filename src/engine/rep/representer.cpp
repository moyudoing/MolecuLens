// =============================================================================
// MolecuLens · engine · rep/representer.cpp —— 表示生成器实现
// 规格来源：主方案 §5.2 / §5.8.2（kDrawTable 分派；配色方案；材质块发射）
// =============================================================================

#include "engine/rep/representer.h"

#include <algorithm>
#include <cmath>

#include "engine/render/render_parameter.h"
#include "engine/style/style_registry.h"
#include "engine/style/quicksurf/quicksurf.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

// —— 风格生成器实现（各风格 cpp；统一签名 StyleDrawFn）——
// 分派经 style_registry 的 styleFor()（先运行时覆盖层、后编译期静态表）——
// 单一事实来源。各 draw* 入口的声明统一由 base_style.h 提供
// （经下方 style_registry.h 转递包含），此处不再重复声明。

namespace {

// —— 中文名（representation.h 声明、此处定义；样式名以 style_registry 为唯一来源）——
const char* representationTypeZhImpl(RepresentationType type) noexcept {
    // 样式名转调 style_registry 的 styleDisplayName（权威中文名，消除 styles_tool 重复表）
    if (const char* name = styleDisplayName(type); name != nullptr) {
        return name;
    }
    return "未知";
}

const char* colorSchemeZhImpl(ColorScheme scheme) noexcept {
    switch (scheme) {
        case ColorScheme::ElementCPK: return "元素 CPK";
        case ColorScheme::Chain:       return "按链";
        case ColorScheme::Residue:     return "按残基";
        case ColorScheme::BFactor:     return "按 BFactor";
        case ColorScheme::Uniform:     return "统一色";
        case ColorScheme::AtomName:    return "按原子名";
        default:                       return "未知";
    }
}

} // namespace

// representation.h 声明的英文/中文名（统一入口，UI 与日志共用）
const char* representationTypeName(RepresentationType type) noexcept {
    return representationTypeZhImpl(type);
}

const char* representationTypeZh(RepresentationType type) noexcept {
    return representationTypeZhImpl(type);
}

const char* colorSchemeName(ColorScheme scheme) noexcept {
    return colorSchemeZhImpl(scheme);
}

const char* colorSchemeZh(ColorScheme scheme) noexcept {
    return colorSchemeZhImpl(scheme);
}

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

bool Representer::lightBackground_ = false;

void Representer::setLightBackground(bool on) {
    lightBackground_ = on;
}

namespace {
// sRGB(0..1) → 线性(0..1)。调色板与 IP 配色都按 sRGB 书写，而颜色附件是 *_SRGB
// （硬件在写入时做 linear→sRGB 编码），所以必须先转线性，否则颜色会被二次编码、整体偏淡。
inline glm::vec4 srgbToLinear(const glm::vec4& c) {
    auto f = [](float v) -> float {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return glm::vec4(f(c.r), f(c.g), f(c.b), c.a);
}
} // namespace

glm::vec4 Representer::colorForAtom(const RepContext& ctx, uint32_t globalAtomIndex) {
    return srgbToLinear(colorForAtomSrgb(ctx, globalAtomIndex));
}

glm::vec4 Representer::colorForAtomSrgb(const RepContext& ctx, uint32_t globalAtomIndex) {
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
            const int32_t rid = st.atoms[localIdx].residueId;
            // 结构是共享的（md.gro：7 个结构 / 2042 个实例），所以链内差异必须靠
            // 实例序号 + 残基号混合，否则同种分子全部同色。
            const uint32_t mix = static_cast<uint32_t>(cid + 1);
            const float h = static_cast<float>(hash32(mix) % 3600) / 3600.0f;
            const float jitter =
                (static_cast<float>(hash32(static_cast<uint32_t>(rid) ^
                                           (static_cast<uint32_t>(ctx.instanceIndex) * 2654435761u)) %
                                    1000) /
                     1000.0f - 0.5f) * 0.20f;
            const float lightness = std::clamp(0.55f + jitter, 0.15f, 0.9f);
            const glm::vec3 rgb = hslToRgb(h, 0.65f, lightness);
            return glm::vec4(rgb, 1.0f);
        }
        case ColorScheme::Residue: {
            const int32_t rid = st.atoms[localIdx].residueId;
            // 同上：结构共享时，残基差异靠 实例序号 + 残基号 共同决定。
            const uint32_t key = static_cast<uint32_t>(rid) ^
                                 (static_cast<uint32_t>(ctx.instanceIndex) * 2246822519u);
            const float h = static_cast<float>(hash32(key) % 3600) / 3600.0f;
            const glm::vec3 rgb = hslToRgb(h, 0.35f, 0.62f);
            return glm::vec4(rgb, 1.0f);
        }
        case ColorScheme::BFactor: {
            const float b = std::clamp(static_cast<float>(st.atoms[localIdx].bfactor) /
                                           20000.0f, 0.0f, 1.0f);
            return glm::vec4(bfactorColor(b), 1.0f);
        }
        case ColorScheme::AtomName: {
            // 原子名（Name）观感：碳青、氮蓝、氧红、氢白、硫黄、磷橙；其余回落元素色。
            const auto& info = elementInfo(st.elements[localIdx]);
            const char* sym = info.symbol != nullptr ? info.symbol : "";
            if (sym[0] == 'C') {
                return glm::vec4(0.55f, 0.85f, 0.88f, 1.0f);
            }
            if (sym[0] == 'N') {
                return glm::vec4(0.25f, 0.35f, 0.95f, 1.0f);
            }
            if (sym[0] == 'O') {
                return glm::vec4(0.95f, 0.20f, 0.20f, 1.0f);
            }
            if (sym[0] == 'H') {
                // 浅色背景（白底导出）时纯白等于隐形，压成中灰
                const float h = lightBackground_ ? 0.45f : 0.95f;
                return glm::vec4(h, h, h, 1.0f);
            }
            if (sym[0] == 'S') {
                return glm::vec4(0.95f, 0.85f, 0.25f, 1.0f);
            }
            if (sym[0] == 'P') {
                return glm::vec4(0.95f, 0.60f, 0.25f, 1.0f);
            }
            return glm::vec4(info.color[0], info.color[1], info.color[2], 1.0f);
        }
        case ColorScheme::Uniform:
        default:
            // 顶点色返回白：统一色由材质 diffuse 决定（着色器 base = diffuse × vColor）。
            // 此前返回 diffuse 会被再乘一次 diffuse，导致选的颜色偏暗（v2.3 修正）。
            return glm::vec4(1.0f, 1.0f, 1.0f, ctx.rep->opacity);
    }
}

void Representer::generate(const Representation& rep,
                           const entity::SceneStructure& scene,
                           const entity::MoleculeInstance& mi,
                           uint32_t instanceIndex,
                           const entity::FrameData& frame,
                           uint64_t frameIndex,
                           const std::vector<bool>& mask,
                           SceneCommandList& out,
                           uint32_t globalAtomBaseOverride) {
    RepContext ctx;
    ctx.scene = &scene;
    ctx.instance = &mi;
    ctx.frame = &frame;
    ctx.rep = &rep;
    ctx.mask = &mask;
    ctx.instanceIndex = instanceIndex;
    ctx.globalAtomBase = globalAtomBaseOverride != UINT32_MAX
                             ? globalAtomBaseOverride
                             : scene.globalAtomBase(instanceIndex);
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

    // 分派：经 style_registry 统一入口 styleFor()（先运行时覆盖层、后编译期静态表）
    if (StyleDrawFn draw = styleFor(rep.type); draw != nullptr) {
        draw(ctx, out);
    }
}

} // namespace moleculens::engine
