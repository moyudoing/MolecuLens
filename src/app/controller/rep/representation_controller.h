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

    // 重命名。**不**置 DIRTY_REPRESENTATION：名字不参与命令流生成，渲染结果与
    // 名称无关；而 .mlscene 保存读的是渲染器绑定的同一份列表（零拷贝视图），
    // 故赋值即可被持久化看见。这里省掉的是一次无谓的全量命令流重建。
    bool setName(int32_t repId, const std::string& name);

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

    // —— ESP 映射表面参数（type == EspSurface 时生效）——
    // 字段语义见 engine/style/espsurface/esp_surface.h；与 surfGridSpacing 等同模式：
    // 写入 Representation 后 touch()，由渲染器在下一次 prepare 时重建命令流。
    bool setEspDensityCubePath(int32_t repId, const std::string& path);
    bool setEspPotentialCubePath(int32_t repId, const std::string& path);
    bool setEspIsovalue(int32_t repId, float iso);
    bool setEspRange(int32_t repId, float vminKcal, float vmaxKcal);
    bool setEspAutoRange(int32_t repId, bool on);
    bool setEspInvert(int32_t repId, bool on);
    bool setEspColormap(int32_t repId, int32_t colormap);

    // 材质（主方案 §6.6）。presetIndex = 选中的材质预设下标，-1 = 自定义
    // （由 UI 在"选了预设"或"手改参数"时分别传入；见 engine/render/render_parameter.h）。
    bool setMaterial(int32_t repId, int32_t presetIndex, const float* ambient,
                     const float* diffuse, const float* specular, float shininess,
                     float metallic, float roughness, float opacity);

    // 便捷：套用某个材质预设（数值取自引擎的 kMaterialPresets 单一事实来源）
    bool setMaterialPreset(int32_t repId, int32_t presetIndex, const float* ambient,
                           const float* diffuse, const float* specular, float shininess,
                           float metallic, float roughness, float opacity);

    [[nodiscard]] const std::vector<engine::Representation>& representations() const;

    // —— UI 编辑目标（样式面板 / 材质面板共用）——
    // 放在控制器而不是各面板：两个面板必须编辑**同一个**表示，各自持一份"选中
    // 下标"就是又一个"两份状态"——正是 export_img_tool 曾经界面值与生效参数
    // 不一致的成因（见 moleculens-fixes）。
    // 契约：返回值恒落在 [0, size())。列表为空时返回 0（调用方须先判空）。
    [[nodiscard]] int32_t activeIndex() const;
    void setActiveIndex(int32_t idx);

    // 装配：把本控制器持有的表示列表绑定给渲染器（此后每次变更由 touch() 重新绑定）。
    // 控制器是表示数据的唯一持有者，渲染器只持只读视图 → 变更不再整表深拷贝。
    void attachToRenderer();

    // 整体替换表示列表（载入 .mlscene 等"整场景替换"用）。
    // 换入后调用 touch()：递增修订号、重新绑定渲染器、置 DIRTY_REPRESENTATION。
    void replaceAll(std::vector<engine::Representation> reps);

    // 事件相关
    void onEvent(const Event&) override;

    // 实例数超过此值 → 新建的表面表示默认走"整体系合并表面"。
    // 依据：md.gro 这类溶剂化体系有上千个分子实例，"每分子一个独立表面"既慢又碎
    // （实测 QuickSurf 846 ms / 316 MB vs 整体系 170 ms / 19.8 MB）。
    // 小体系（单个蛋白/小分子）保持"每实例一个表面"的原行为。
    static constexpr uint32_t kSystemSurfaceInstanceThreshold = 32;

private:
    AppServices& svc_;
    std::vector<engine::Representation> reps_;
    uint64_t revision_ = 0;   // 表示修订号（每次变更递增；渲染器据此判断需否重建）
    int32_t activeIndex_ = 0; // UI 编辑目标（见 activeIndex()）
    void touch();
    void clampActiveIndex();  // 增删后把 activeIndex_ 拉回合法区间
};

} // namespace moleculens::app
