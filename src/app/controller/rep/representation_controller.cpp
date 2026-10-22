// =============================================================================
// MolecuLens · app · controller/rep/representation_controller.cpp
// =============================================================================

#include "app/i18n/language.h"
#include "app/controller/rep/representation_controller.h"

#include <algorithm>
#include <cmath>

#include "engine/render/renderer.h"
#include "engine/style/espsurface/esp_surface.h"   // EspColormap::Count（色标方案合法区间）
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::app {

RepresentationController::RepresentationController(AppServices& svc) : svc_(svc) {

    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }

    // 默认表示（主方案 §7.7：加载即显示球棍）
    reps_.push_back(engine::Representation{});
    reps_.back().name = "ballstick";
    reps_.back().type = engine::RepresentationType::BallAndStick;
}

int32_t RepresentationController::addRepresentation(const std::string& name,
                                                    engine::RepresentationType type,
                                                    const std::string& selection,
                                                    engine::ColorScheme scheme) {
    engine::Representation rep;
    rep.name = name.empty() ? ("rep_" + std::to_string(reps_.size())) : name;
    rep.type = type;
    rep.selection = selection;
    rep.colorScheme = scheme;
    // v2.8：新建分子表面 / 快速表面时默认「整体系合并」——与默认只出一张
    // 连续表面的观感一致，也避免多实例体系里 2000+ 个水分子各生成一张小球壳。
    if (type == engine::RepresentationType::Surface ||
        type == engine::RepresentationType::QuickSurf) {
        rep.systemSurface = true;
    }
    reps_.push_back(rep);
    clampActiveIndex();
    touch();
    return static_cast<int32_t>(reps_.size()) - 1;
}

bool RepresentationController::removeRepresentation(int32_t repId) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_.erase(reps_.begin() + repId);
    // 删掉的正好是编辑目标或其之前的项 → 下标要跟着左移，否则编辑对象会
    // 静默跳到相邻表示上（用户看到的是"删了一个，另一个参数变了"）
    if (activeIndex_ > repId) {
        --activeIndex_;
    }
    clampActiveIndex();
    touch();
    return true;
}

bool RepresentationController::setName(int32_t repId, const std::string& name) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].name = name;
    // 不调 touch()：名称不参与命令流生成（渲染结果与名字无关），而 .mlscene
    // 保存读的是渲染器已绑定的同一份列表，故赋值即可被持久化看见。
    return true;
}

int32_t RepresentationController::activeIndex() const {
    if (reps_.empty()) {
        return 0;
    }
    const int32_t last = static_cast<int32_t>(reps_.size()) - 1;
    return activeIndex_ < 0 ? 0 : (activeIndex_ > last ? last : activeIndex_);
}

void RepresentationController::setActiveIndex(int32_t idx) {
    activeIndex_ = idx;
    clampActiveIndex();
}

void RepresentationController::clampActiveIndex() {
    activeIndex_ = activeIndex();   // 复用同一套钳制逻辑（单一事实来源）
}

bool RepresentationController::setType(int32_t repId, engine::RepresentationType type) {

    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    auto& rep = reps_[static_cast<size_t>(repId)];
    const bool wasSurface = rep.type == engine::RepresentationType::Surface ||
                            rep.type == engine::RepresentationType::QuickSurf;
    rep.type = type;

    // 体系级表面的**合理默认**：溶剂化/多实例体系下"每分子一个表面"是病态的
    // —— 实测 md.gro（2042 个实例）：QuickSurf 846 ms / 命令流 316 MB，
    // 而整体系合并只需 170 ms / 19.8 MB（5× 与 16×）。
    // 仅在**从非表面样式切到表面样式**时给默认值：这样
        //  · 用户随后手动切换"整体系合并表面"不会被覆盖（同类型内不再改）；
    //  · 每次切到表面样式都从合理默认开始（该标志本就是随样式而定的）。
    if (!wasSurface &&
        (type == engine::RepresentationType::Surface ||
         type == engine::RepresentationType::QuickSurf)) {
        uint32_t instances = 0;
        if (svc_.scene != nullptr) {
            instances = static_cast<uint32_t>(svc_.scene->instances().size());
        }
        rep.systemSurface = instances > kSystemSurfaceInstanceThreshold;
        if (rep.systemSurface) {
            spdlog::info("[rep] {} 实例的体系 -> 默认启用整体系合并表面"
                         "（可在面板关闭）",
                         instances);
        }
    }
    touch();
    return true;

}

bool RepresentationController::setSelection(int32_t repId, const std::string& selection) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].selection = selection;
    touch();
    return true;
}

bool RepresentationController::setColorScheme(int32_t repId, engine::ColorScheme scheme) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].colorScheme = scheme;
    touch();
    return true;
}

bool RepresentationController::setRadiusScale(int32_t repId, float scale) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].radiusScale = scale;
    touch();
    return true;
}

bool RepresentationController::setBondScale(int32_t repId, float scale) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].bondScale = scale;
    touch();
    return true;
}

bool RepresentationController::setVisible(int32_t repId, bool visible) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].visible = visible;
    touch();
    return true;
}

bool RepresentationController::setSurfIsovalue(int32_t repId, float v) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].surfIsovalue = v;
    touch();
    return true;
}

bool RepresentationController::setSurfGridSpacing(int32_t repId, float s) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].surfGridSpacing = s;
    touch();
    return true;
}

bool RepresentationController::setSurfSmooth(int32_t repId, int iterations) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].surfSmooth = std::clamp(iterations, 0, 3);
    touch();
    return true;
}

bool RepresentationController::setSystemSurface(int32_t repId, bool on) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].systemSurface = on;
    touch();
    return true;
}

bool RepresentationController::setSurfaceDrawMode(int32_t repId, uint8_t mode) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].surfaceDrawMode = std::min<uint8_t>(mode, 2);
    touch();
    return true;
}

bool RepresentationController::setInstanceRangeMode(int32_t repId, uint8_t mode) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].rangeMode = std::min<uint8_t>(
        mode, static_cast<uint8_t>(engine::Representation::InstanceRangeMode::Stride));
    touch();
    return true;
}

bool RepresentationController::setInstanceRangeType(int32_t repId,
                                                    const std::string& typeName) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].rangeType = typeName;
    touch();
    return true;
}

bool RepresentationController::setInstanceRangeFraction(int32_t repId, float fraction) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size() ||
        !std::isfinite(fraction)) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].rangeFraction = std::clamp(fraction, 0.0f, 1.0f);
    touch();
    return true;
}

bool RepresentationController::setInstanceRangeStride(int32_t repId, uint32_t stride,
                                                      uint32_t offset) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    auto& rep = reps_[static_cast<size_t>(repId)];
    rep.rangeStride = std::max(1u, stride);
    rep.rangeOffset = offset % rep.rangeStride;
    touch();
    return true;
}

// —— ESP 映射表面参数 ——
// 与 surfIsovalue/surfGridSpacing 同一约定：写字段 + touch()；不在此处校验文件
// 是否存在（路径可能来自 .mlscene，且渲染层 buildEspSurface 自己会报错并不绘制）。
bool RepresentationController::setEspDensityCubePath(int32_t repId,
                                                     const std::string& path) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].espDensityCubePath = path;
    touch();
    return true;
}

bool RepresentationController::setEspPotentialCubePath(int32_t repId,
                                                       const std::string& path) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].espPotentialCubePath = path;
    touch();
    return true;
}

bool RepresentationController::setEspIsovalue(int32_t repId, float iso) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size() || !std::isfinite(iso)) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].espIsovalue = iso;
    touch();
    return true;
}

bool RepresentationController::setEspRange(int32_t repId, float vminKcal,
                                           float vmaxKcal) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size() ||
        !std::isfinite(vminKcal) || !std::isfinite(vmaxKcal)) {
        return false;
    }
    // 上限必须大于下限：否则 valueToT 的 span 退化，色标/着色退化为单色。
    // 这里做一次最小修正（+1e-3）而不是拒绝写入——UI 拖动上下限时不该卡住。
    auto& rep = reps_[static_cast<size_t>(repId)];
    rep.espVminKcal = vminKcal;
    rep.espVmaxKcal = vmaxKcal <= vminKcal ? vminKcal + 1e-3f : vmaxKcal;
    touch();
    return true;
}

bool RepresentationController::setEspAutoRange(int32_t repId, bool on) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].espAutoRange = on;
    touch();
    return true;
}

bool RepresentationController::setEspInvert(int32_t repId, bool on) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    reps_[static_cast<size_t>(repId)].espInvert = on;
    touch();
    return true;
}

bool RepresentationController::setEspColormap(int32_t repId, int32_t colormap) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    // 合法区间来自引擎枚举（0..Count-1）；越界值会让 drawEspSurface 的
    // sanitizeColormap 静默回退 rwb，UI 下拉却回显一个不存在的项。
    const int32_t last = static_cast<int32_t>(engine::EspColormap::Count) - 1;
    reps_[static_cast<size_t>(repId)].espColormap = std::clamp(colormap, 0, last);
    touch();
    return true;
}

bool RepresentationController::setMaterial(int32_t repId, int32_t presetIndex,
                                           const float* ambient, const float* diffuse,
                                           const float* specular, float shininess,
                                           float metallic, float roughness,
                                           float opacity) {
    if (repId < 0 || static_cast<size_t>(repId) >= reps_.size()) {
        return false;
    }
    auto& rep = reps_[static_cast<size_t>(repId)];
    for (int i = 0; i < 4; ++i) {
        rep.ambient[i] = ambient[i];
        rep.diffuse[i] = diffuse[i];
        rep.specular[i] = specular[i];
    }
    rep.shininess = shininess;
    rep.metallic = metallic;
    rep.roughness = roughness;
    rep.opacity = opacity;
    rep.materialPreset = presetIndex;   // 记录选中项，使下拉精确回显
    touch();
    return true;
}

// 与 setMaterial 同实现：区分两个入口只是为了让调用点自述意图
// （"套用预设" vs "手改参数并置自定义"），避免在调用点散落魔法值 -1。
bool RepresentationController::setMaterialPreset(int32_t repId, int32_t presetIndex,
                                                 const float* ambient,
                                                 const float* diffuse,
                                                 const float* specular, float shininess,
                                                 float metallic, float roughness,
                                                 float opacity) {
    return setMaterial(repId, presetIndex, ambient, diffuse, specular, shininess, metallic,
                       roughness, opacity);
}

const std::vector<engine::Representation>&
RepresentationController::representations() const {
    return reps_;
}

void RepresentationController::attachToRenderer() {
    if (svc_.renderer) {
        svc_.renderer->bindRepresentations(&reps_, revision_);
    }
}

void RepresentationController::replaceAll(std::vector<engine::Representation> reps) {
    reps_ = std::move(reps);
    activeIndex_ = 0;   // 整表换入（载入 .mlscene）→ 旧的编辑下标不再指代同一表示
    clampActiveIndex();
    touch();   // 递增 revision、重新绑定、置脏并广播
}

void RepresentationController::touch() {
    // 只递增修订号并把只读视图重新绑定，不再整表拷贝（拖滑杆时此前每帧一次深拷贝）
    ++revision_;
    attachToRenderer();
    if (svc_.scene) {
        svc_.scene->setDirty(entity::DIRTY_REPRESENTATION);
    }
    if (svc_.bus) {
        svc_.bus->publish({EventType::RepresentationChanged, 0});
    }
}

void RepresentationController::onEvent(const Event&) {}

} // namespace moleculens::app
