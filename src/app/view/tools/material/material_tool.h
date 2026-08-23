// =============================================================================
// MolecuLens · app · view/tools/material/material_tool.h —— 材质选择面板
// 规格来源：主方案 §6.6.2/§7.9（8 套预设：默认/金属/光泽/哑光/半透明/玻璃/
//           粗糙金属/塑料；detectPreset 容差匹配；手动改参回"自定义"）
// =============================================================================

#pragma once

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class RepresentationController;

class MaterialTool : public BaseToolView {
public:
    const char* getTitle() const override { return "材质选择";
    }
    void render(DataManager* dm, MolScene* scene) override;

    // 控制器注入（ViewControl::svcRep 装配）
    void setRepController(RepresentationController* ctl) { repCtl_ = ctl; }

    // 预设表（8 套，与主方案 §6.6.2 同源）
    struct Preset {
        const char* name;
        float ambient[4], diffuse[4], specular[4];
        float shininess, metallic, roughness, opacity;
    };
    static const Preset* presets();
    static int detectPreset(const Preset& cur, float tolerance);
    static void presetParams(int index, float* ambient, float* diffuse, float* specular,
                             float* shininess, float* metallic, float* roughness,
                             float* opacity);

private:
    RepresentationController* repCtl_ = nullptr;
};

} // namespace moleculens::app
