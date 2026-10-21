// =============================================================================
// MolecuLens · app · view/chrome/window_chrome.cpp —— 窗口控制与标题栏图标实现
// 规格来源：主方案 §7.3
// =============================================================================

#include "app/view/chrome/window_chrome.h"

#include <cmath>

#include <GLFW/glfw3.h>

#include "app/view/theme/ui_theme.h"   // ui::C_TEXT / ui::C_BG（随主题变化）

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>

// 必须在 glfw3native.h **之前**定义，否则 glfwGetWin32Window 不声明
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace moleculens::app::chrome {

namespace {

#ifdef _WIN32
// 所在监视器的工作区（排除任务栏）；失败回退到系统工作区
RECT monitorWorkArea(HWND hwnd) {
    RECT work{};
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (mon && GetMonitorInfo(mon, &mi)) {
        work = mi.rcWork;
    } else {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    }
    return work;
}
#endif

} // namespace

bool windowIsMaximized(GLFWwindow* window, const MaximizeState& st) {
#ifdef _WIN32
    HWND hwnd = window ? glfwGetWin32Window(window) : nullptr;
    if (!hwnd) {
        return st.maximized;
    }
    // 原生最大化（WS_MAXIMIZE 置位）
    if (::IsZoomed(hwnd)) {
        return true;
    }
    // 自绘最大化：SetWindowPos 铺满工作区（不置 WS_MAXIMIZE 位，IsZoomed 为假），
    // 用"当前矩形 ≈ 所在监视器工作区"判断，外部还原后矩形变化 → 状态自动同步。
    RECT cur{};
    if (!GetWindowRect(hwnd, &cur)) {
        return st.maximized;
    }
    const RECT work = monitorWorkArea(hwnd);
    constexpr int kTol = 4;   // 像素容差（任务栏/边框/缩放误差）
    return (std::abs(cur.left - work.left) <= kTol &&
            std::abs(cur.top - work.top) <= kTol &&
            std::abs(cur.right - work.right) <= kTol &&
            std::abs(cur.bottom - work.bottom) <= kTol);
#else
    (void)window;
    return st.maximized;
#endif
}

void toggleMaximize(GLFWwindow* window, MaximizeState& st) {
    if (!window) {
        return;
    }
#ifdef _WIN32
    // 无边框窗口用 glfwMaximizeWindow/glfwRestoreWindow 时 GLFW 内部尺寸不同步
    // （ImGui DisplaySize 停留在旧值，UI 不铺满、还原失效）。改用 Win32 手动
    // 最大化到工作区 / 还原到记录位置，SetWindowPos → WM_SIZE → GLFW 同步尺寸。
    if (HWND hwnd = glfwGetWin32Window(window)) {
        if (windowIsMaximized(window, st)) {
            // 还原
            if (st.restoreValid) {
                SetWindowPos(hwnd, nullptr, st.restoreL, st.restoreT,
                             st.restoreR - st.restoreL, st.restoreB - st.restoreT,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                ShowWindow(hwnd, SW_RESTORE);
            }
        } else {
            // 最大化：先记录当前矩形，再铺满工作区（排除任务栏）
            RECT cur{};
            if (GetWindowRect(hwnd, &cur)) {
                st.restoreValid = true;
                st.restoreL = cur.left;
                st.restoreT = cur.top;
                st.restoreR = cur.right;
                st.restoreB = cur.bottom;
            }
            const RECT work = monitorWorkArea(hwnd);
            SetWindowPos(hwnd, HWND_TOP, work.left, work.top, work.right - work.left,
                         work.bottom - work.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        st.maximized = windowIsMaximized(window, st);   // 同步到操作后的真实状态
        return;
    }
#endif
    // 非 Windows 兜底
    if (st.maximized) {
        glfwRestoreWindow(window);
        st.maximized = false;
    } else {
        glfwMaximizeWindow(window);
        st.maximized = true;
    }
}

void drawWindowGlyph(const ImVec2& pos, const ImVec2& size, WindowGlyph glyph) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 走主题正文字色（此前写死 IM_COL32 → 切主题时标题栏图标不跟随）
    const ImU32 col = ImGui::GetColorU32(ui::C_TEXT);
    const float t = 2.0f;
    const ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    switch (glyph) {
        case WindowGlyph::Minimize:
            dl->AddLine(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x + 6.0f, c.y), col, t);
            break;
        case WindowGlyph::Maximize:
            dl->AddRect(ImVec2(c.x - 6.0f, c.y - 5.0f), ImVec2(c.x + 6.0f, c.y + 5.0f),
                        col, 0.0f, 0, t);
            break;
        case WindowGlyph::Restore: {
            // 两个错位方框（后框 + 前景底 + 前框），表示"还原"
            const ImVec2 a(c.x - 3.0f, c.y - 4.0f), b(c.x + 6.0f, c.y + 5.0f);
            const ImVec2 a2(c.x - 6.0f, c.y - 5.0f), b2(c.x + 3.0f, c.y + 4.0f);
            dl->AddRect(a2, b2, col, 0.0f, 0, t);
            // 前景方框填充 = 标题栏底色（盖住后框被遮部分）；同样走主题
            dl->AddRectFilled(a, b, ImGui::GetColorU32(ui::C_BG));
            dl->AddRect(a, b, col, 0.0f, 0, t);
            break;
        }
        case WindowGlyph::Close:
            dl->AddLine(ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 5.0f, c.y + 5.0f),
                        col, t);
            dl->AddLine(ImVec2(c.x - 5.0f, c.y + 5.0f), ImVec2(c.x + 5.0f, c.y - 5.0f),
                        col, t);
            break;
    }
}

} // namespace moleculens::app::chrome
