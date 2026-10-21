// =============================================================================
// MolecuLens v2 · app/workbench/module.h —— 检查器模块契约
//
// 一个模块 = 左侧导航栏的一项 + 右侧检查器的一页。新增功能只需：
//   1) 实现 IInspectorModule；2) 在 Workbench::init 里 make + add。
// 模块只通过 Workbench 暴露的服务访问控制器/场景，不得直接持有 Renderer。
// =============================================================================

#pragma once

namespace moleculens::app {

class Workbench;

class IInspectorModule {
public:
    virtual ~IInspectorModule() = default;

    // 稳定 ID（持久化当前页用；不可随语言变化）
    [[nodiscard]] virtual const char* id() const = 0;
    // 导航栏标签（中文源串，调用方用 tr() 翻译）
    [[nodiscard]] virtual const char* label() const = 0;
    // 导航栏副标题（当前页顶部说明，可为空）
    [[nodiscard]] virtual const char* subtitle() const { return ""; }

    // 渲染右侧检查器内容（已在滚动容器内）
    virtual void render(Workbench& wb) = 0;

    // 切换到本模块时回调（懒加载/校验）
    virtual void onActivate(Workbench&) {}
};

} // namespace moleculens::app
