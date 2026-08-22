// =============================================================================
// MolecuLens · app · controller/rep/representation_controller.h —— 表示控制器
// 规格来源：主方案 §5.8.4（样式/配色/半径/材质变更 → DIRTY_REPRESENTATION）
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/rep/representation.h"

namespace moleculens::app {

class RepresentationController : public Subscriber {
public:
    explicit RepresentationController(AppServices& svc);

    // 表示列表管理（成功后发布 RepresentationChanged）
    int32_t addRepresentation(const std::string& name,
                              engine::RepresentationType type,
                              const std::string& selection,
                              engine::ColorScheme scheme);
    bool removeRepresentation(int32_t repId);
    bool setType(int32_t repId, engine::RepresentationType type);
    bool setSelection(int32_t repId, const std::string& selection);
    bool setColorScheme(int32_t repId, engine::ColorScheme scheme);
    bool setRadiusScale(int32_t repId, float scale);
    bool setVisible(int32_t repId, bool visible);
    // 材质（主方案 §6.6）
    bool setMaterial(int32_t repId, const float* ambient, const float* diffuse,
                     const float* specular, float shininess, float metallic,
                     float roughness, float opacity);

    const std::vector<engine::Representation>& representations() const;

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
    std::vector<engine::Representation> reps_;
    void touch();
};

} // namespace moleculens::app
