// =============================================================================
// MolecuLens · app · view/chrome/window_chrome.h —— 自绘标题栏：窗口控制 + 图标
// 规格来源：主方案 §7.3（自绘标题栏的最小化/最大化/还原/关闭）
//
// 从 view_control.cpp 抽出的独立关注点：**无边框窗口的最大化/还原状态机**（Win32
// 手工 SetWindowPos，绕开 GLFW 无边框窗口的尺寸不同步）与**窗口按钮图标绘制**。
// 二者都不依赖 ViewControl 的其余状态，只依赖窗口句柄 + 一份还原矩形记录。
//
// ⚠ 注意：真正的标题栏**布局**仍留在 ViewControl::renderMainMenuBar —— 自绘控件
//   必须画在 BeginMenuBar() 之内（放在 EndMenuBar 之后会被裁剪，见 moleculens-fixes）。
// =============================================================================

#pragma once

#include <imgui.h>

struct GLFWwindow;

namespace moleculens::app::chrome {

// 自绘标题栏的窗口按钮图标（用绘制列表画，不依赖字体字形：最大化/还原需随状态切换）
enum class WindowGlyph { Minimize, Maximize, Restore, Close };

// 最大化状态 + 还原矩形（调用方持有的持久状态）
struct MaximizeState {
    bool maximized = false;
    bool restoreValid = false;
    int  restoreL = 0, restoreT = 0, restoreR = 0, restoreB = 0;
};

// 查询窗口的**真实**最大化状态。
// 原生最大化（WS_MAXIMIZE）用 IsZoomed；自绘最大化（SetWindowPos 铺满工作区，
// 不置 WS_MAXIMIZE）用"当前矩形 ≈ 所在监视器工作区"判断（4px 容差），
// 因此外部最大化/还原后状态能自动同步，不会过期。
// 非 Windows 平台回退到 st.maximized。
bool windowIsMaximized(GLFWwindow* window, const MaximizeState& st);

// 最大化 ⇄ 还原切换，并把结果状态同步进 st.maximized。
void toggleMaximize(GLFWwindow* window, MaximizeState& st);

// 在 pos/size 指定的矩形内绘制窗口按钮图标（颜色取主题正文字色）。
void drawWindowGlyph(const ImVec2& pos, const ImVec2& size, WindowGlyph glyph);

} // namespace moleculens::app::chrome
