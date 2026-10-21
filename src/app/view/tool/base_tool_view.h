// =============================================================================
// Moleculens · app · view/tool/base_tool_view.h —— 工具面板抽象
// 规格来源：主方案 §7.1（BaseToolView：getTitle/getVisible/render；
//           加面板 = 写一个类 + addModule 一行注册）
// =============================================================================

#pragma once

#include <string>

#include <imgui.h>

#include "app/i18n/language.h"              // tr（面板标题可翻译）
#include "app/view/dock/dock_layout.h"      // panel::windowLabel / 稳定 ID

namespace moleculens::app {

class DataManager;
class MolScene;

// 工具面板抽象（主方案 §7.1）
class BaseToolView {
public:
    virtual ~BaseToolView() = default;

    // 面板标题（显示名，**可翻译**；不要用它当窗口 ID）
    [[nodiscard]] virtual const char* getTitle() const = 0;
    // 面板窗口的**稳定 ID**（见 dock_layout.h panel:: 常量）。
    // 必须与 buildDefaultLayout 里的 DockBuilderDockWindow 用同一个 —— 两者不一致
    // 会让该面板静默脱离停靠区（漂浮在左上角），且不会有任何报错。
    [[nodiscard]] virtual const char* getStableId() const = 0;
    virtual bool* getVisible() { return nullptr; }   // nullptr = 常显
    virtual void render(DataManager* dm, MolScene* scene) = 0;

    // 面板窗口标签：`显示名###稳定id`。
    // 显示名走 tr()（切语言时标题跟着变），ID 恒定（Dock 布局/imgui.ini 的键不变）。
    [[nodiscard]] std::string windowLabel() const {
        return dock::windowLabel(i18n::tr(getTitle()), getStableId());
    }

    // 面板窗口渲染（ViewControl 调度调用）
    void draw(DataManager* dm, MolScene* scene) {
        const std::string label = windowLabel();
        if (ImGui::Begin(label.c_str(), getVisible(), ImGuiWindowFlags_NoCollapse)) {
            render(dm, scene);
        }
        ImGui::End();
    }
};

} // namespace moleculens::app
