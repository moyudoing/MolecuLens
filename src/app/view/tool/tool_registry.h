// =============================================================================
// MolecuLens · app · view/tool/tool_registry.h —— 工具面板注册表
// 规格来源：主方案 §7.1（modules_ 路由；addModule 一行注册）
// =============================================================================

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class ToolRegistry {
public:
    void addModule(std::unique_ptr<BaseToolView> tool) {
        modules_.push_back(std::move(tool));
    }

    void renderAll(DataManager* dm, MolScene* scene) {
        for (auto& tool : modules_) {
            tool->draw(dm, scene);
        }
    }

    [[nodiscard]] const std::vector<std::unique_ptr<BaseToolView>>& modules() const { return modules_; }

private:
    std::vector<std::unique_ptr<BaseToolView>> modules_;
};

} // namespace moleculens::app
