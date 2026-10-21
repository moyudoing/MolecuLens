// =============================================================================
// MolecuLens · app · controller/render/render_controller.cpp
// =============================================================================

#include "app/controller/render/render_controller.h"

#include <algorithm>

#include "engine/render/render_parameter.h"   // LightParams（角度↔方向换算复用）
#include "engine/render/renderer.h"           // Renderer::setRenderStyle（风格直达后端）
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

RenderController::RenderController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

bool RenderController::setBackgroundColor(float r, float g, float b) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.bgColor[0] = r;
    props.bgColor[1] = g;
    props.bgColor[2] = b;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    if (svc_.bus) {
        svc_.bus->publish({EventType::StructureChanged, entity::DIRTY_PROPERTIES});
    }
    return true;
}

bool RenderController::setAmbient(float ambient) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().ambient = ambient;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setFog(float near, float far, int mode, float density) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.fog[0] = near;
    props.fog[1] = far;
    props.fogMode = static_cast<float>(mode);
    props.fogDensity = density;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setGlobalScale(float scale) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().scale = scale;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setSkyColor(float r, float g, float b) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.skyColor[0] = r;
    props.skyColor[1] = g;
    props.skyColor[2] = b;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setGroundColor(float r, float g, float b) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.groundColor[0] = r;
    props.groundColor[1] = g;
    props.groundColor[2] = b;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setAmbientColor(float r, float g, float b) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.ambientColor[0] = r;
    props.ambientColor[1] = g;
    props.ambientColor[2] = b;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setDirLightAngles(float azimuth, float elevation) {
    if (!svc_.scene) {
        return false;
    }
    // 方向在 SceneProperties 里存三分量（与 mlscene/桥接一致）；角度↔分量换算
    // 复用 LightParams 的版本，避免两处各写一份三角函数。
    engine::LightParams tmp;
    tmp.setDirLightAngles(azimuth, elevation);
    auto& props = svc_.scene->properties();
    for (int i = 0; i < 3; ++i) {
        props.dirLightDir[i] = tmp.dirLightDir[i];
    }
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setDirLightColor(float r, float g, float b) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.dirLightColor[0] = r;
    props.dirLightColor[1] = g;
    props.dirLightColor[2] = b;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setDirLightIntensity(float intensity) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().dirLightIntensity =
        std::clamp(intensity, 0.0f, 4.0f);
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setDirLightEnabled(bool enabled) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().dirLightEnabled = enabled;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setShadowsEnabled(bool enabled) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().shadowsEnabled = enabled;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setShadowStrength(float strength) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().shadowStrength = std::clamp(strength, 0.0f, 1.0f);
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

void RenderController::dirLightAngles(float* azimuth, float* elevation) const {
    if (!svc_.scene || azimuth == nullptr || elevation == nullptr) {
        return;
    }
    engine::LightParams tmp;
    const auto& props = svc_.scene->properties();
    tmp.dirLightDir = glm::vec3(props.dirLightDir[0], props.dirLightDir[1],
                                props.dirLightDir[2]);
    tmp.dirLightAngles(azimuth, elevation);
}

void RenderController::setRenderStyle(const engine::RenderStyle& style) {
    style_ = style;
    if (svc_.renderer != nullptr) {
        svc_.renderer->setRenderStyle(style_);
    }
}

void RenderController::applyRenderStylePreset(engine::RenderStylePreset preset) {
    stylePreset_ = preset;
    setRenderStyle(engine::renderStylePreset(preset));
    if (!svc_.scene) {
        return;
    }
    auto& props = svc_.scene->properties();
    switch (preset) {
        case engine::RenderStylePreset::Publication:
            // 出版图：白底 + 高调柔光 + 关阴影。四者必须一起改 —— 只加描边
            // 而不换背景，得到的是"脏边"而不是插图。
            props.bgColor[0] = props.bgColor[1] = props.bgColor[2] = 1.0f;
            props.skyColor[0] = props.skyColor[1] = props.skyColor[2] = 1.0f;
            props.groundColor[0] = 0.90f;
            props.groundColor[1] = 0.90f;
            props.groundColor[2] = 0.94f;
            props.ambientColor[0] = 0.95f;
            props.ambientColor[1] = 0.95f;
            props.ambientColor[2] = 1.0f;
            props.ambient = 0.34f;
            props.dirLightIntensity = 0.55f;
            props.dirLightEnabled = true;
            props.shadowsEnabled = false;
            break;
        case engine::RenderStylePreset::Soft:
            props.ambient = std::max(props.ambient, 0.55f);
            break;
        case engine::RenderStylePreset::Vivid:
            props.ambient = std::min(props.ambient, 0.38f);
            break;
        case engine::RenderStylePreset::Default:
        default:
            break;
    }
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    if (svc_.bus) {
        svc_.bus->publish({EventType::StructureChanged, entity::DIRTY_PROPERTIES});
    }
}

bool RenderController::setClipEnabled(bool enabled) {
    if (!svc_.scene) {
        return false;
    }
    svc_.scene->properties().clipEnabled = enabled;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setClipAxis(int axis, float offset) {
    static const glm::vec3 kNormals[6] = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f},
    };
    if (!svc_.scene) {
        return false;
    }
    const glm::vec3 n = kNormals[std::clamp(axis, 0, 5)];
    auto& props = svc_.scene->properties();
    props.clipPlane[0] = n.x;
    props.clipPlane[1] = n.y;
    props.clipPlane[2] = n.z;
    // 保留侧为 dot(p,n) <= offset → 平面常数 d = -offset
    props.clipPlane[3] = -offset;
    props.clipEnabled = true;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

bool RenderController::setClipOffset(float offset) {
    if (!svc_.scene) {
        return false;
    }
    auto& props = svc_.scene->properties();
    props.clipPlane[3] = -offset;
    svc_.scene->setDirty(entity::DIRTY_PROPERTIES);
    return true;
}

void RenderController::clipState(bool* enabled, glm::vec3* normal,
                                 float* offset) const {
    if (!svc_.scene) {
        return;
    }
    const auto& props = svc_.scene->properties();
    if (enabled != nullptr) {
        *enabled = props.clipEnabled;
    }
    if (normal != nullptr) {
        *normal = glm::vec3(props.clipPlane[0], props.clipPlane[1], props.clipPlane[2]);
    }
    if (offset != nullptr) {
        *offset = -props.clipPlane[3];
    }
}

void RenderController::onEvent(const Event&) {}

} // namespace moleculens::app
