// =============================================================================
// MolecuLens · app · view/tool/base_tool_view.h —— 工具面板抽象
// 规格来源：主方案 §7.1（BaseToolView：getTitle/getVisible/render；
//           加面板 = 写一个类 + addModule 一行注册）
// =============================================================================

#pragma once

#include <imgui.h>

namespace moleculens::app {

class DataManager;
class MolScene;

// 工具面板抽象（主方案 §7.1）
class BaseToolView {
public:
    virtual ~BaseToolView() = default;

    virtual const char* getTitle() const = 0;
    virtual bool* getVisible() { return nullptr; }   // nullptr = 常显
    virtual void render(DataManager* dm, MolScene* scene) = 0;

    // 面板窗口渲染（ViewControl 调度调用）
    void draw(DataManager* dm, MolScene* scene) {
        const char* title = getTitle();
        if (ImGui::Begin(title, getVisible(), ImGuiWindowFlags_NoCollapse)) {
            render(dm, scene);
        }
        ImGui::End();
    }
};

} // namespace moleculens::app
