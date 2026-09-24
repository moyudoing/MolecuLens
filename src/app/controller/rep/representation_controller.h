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

    bool removeRepresentation(int32_t repId);  // 移除

    bool setType(int32_t repId, engine::RepresentationType type);

    bool setSelection(int32_t repId, const std::string& selection);
    bool setColorScheme(int32_t repId, engine::ColorScheme scheme);
    bool setRadiusScale(int32_t repId, float scale);
    bool setBondScale(int32_t repId, float scale);
    bool setVisible(int32_t repId, bool visible);

    // QuickSurf 参数（等值面阈值 / 网格间距）
    bool setSurfIsovalue(int32_t repId, float v);
    bool setSurfGridSpacing(int32_t repId, float s);

    // 体系级表面：整体系（含溶剂分子）一个整体表面（Surface/QuickSurf 用）
    bool setSystemSurface(int32_t repId, bool on);

    // 材质（主方案 §6.6）
    bool setMaterial(int32_t repId, const float* ambient, const float* diffuse,
                     const float* specular, float shininess, float metallic,
                     float roughness, float opacity);

    [[nodiscard]] const std::vector<engine::Representation>& representations() const;

    // 装配：把本控制器持有的表示列表绑定给渲染器（此后每次变更由 touch() 重新绑定）。
    // 控制器是表示数据的唯一持有者，渲染器只持只读视图 → 变更不再整表深拷贝。
    void attachToRenderer();

    // 整体替换表示列表（载入 .mlscene 等"整场景替换"用）。
    // 换入后调用 touch()：递增修订号、重新绑定渲染器、置 DIRTY_REPRESENTATION。
    void replaceAll(std::vector<engine::Representation> reps);

    // 事件相关
    void onEvent(const Event&) override;

private:
    AppServices& svc_;
    std::vector<engine::Representation> reps_;
    uint64_t revision_ = 0;   // 表示修订号（每次变更递增；渲染器据此判断需否重建）
    void touch();
};

} // namespace moleculens::app
