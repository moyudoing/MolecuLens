// =============================================================================
// MolecuLens v2 · app/workbench/style/aurora.h —— Aurora 设计系统
//
// v2 的单一视觉事实来源：调色板、布局度量、基础控件。
// 与旧 UITheme 的分工：
//   · UITheme 仍是**字体加载 + 主题持久化键**的入口（ViewControl 初始化时调用）；
//   · Aurora 负责**工作台的视觉语言**（浅色 "Aurora Lab" 默认 + 深色变体）与控件。
// 颜色/尺寸集中在此，面板模块不得自带魔法数（改一处即全局一致）。
// =============================================================================

#pragma once

#include <cstddef>

#include <imgui.h>

namespace moleculens::app::aurora {

// 当前调色板（浅色/深色各一套；切换时整体替换）
struct Palette {
    ImVec4 bg;           // 应用背景（工作台最底层）
    ImVec4 panel;        // 卡片/面板
    ImVec4 panelAlt;     // 次级面板（左侧导航底、悬浮条）
    ImVec4 panelSunken;  // 视口/输入框的凹陷底
    ImVec4 border;       // 常规描边
    ImVec4 borderStrong; // 悬浮/聚焦描边
    ImVec4 text;         // 正文
    ImVec4 textMuted;    // 次要文字
    ImVec4 textFaint;    // 极弱（禁用、提示）
    ImVec4 accent;       // 主色（蓝）
    ImVec4 accentHi;     // 主色高亮（hover/滑块）
    ImVec4 accentSoft;   // 主色淡底（hover/弱选中）
    ImVec4 onAccent;     // 主色上的文字
    // v2.14：选中态固定高饱和蓝 + 白前景。旧实现用 16% 主色的 accentSoft，
    // 在浅色主题或粉色自定义主色下几乎看不见；导航、列表行、分段按钮统一走这两个字段。
    ImVec4 selection;
    ImVec4 onSelection;
    ImVec4 ok, warn, danger;
    ImVec4 gradTop, gradBottom;   // 视口空背景渐变
    bool dark = false;
};

const Palette& palette();
bool isDark();
void apply(bool dark);   // 写入 ImGui style（颜色 + 圆角/间距）
void toggleDark();
// 主题强调色覆盖（v2.3）：主色/悬停/淡底/前景色由该颜色推导；apply() 重建后仍保留。
// 传 alpha=1 的颜色；clearAccent() 回到深/浅主题的内置蓝色。
void setAccent(const ImVec4& accent);
void clearAccent();
bool hasCustomAccent();
ImVec4 accentColor();

// —— 布局度量（px，1280×800 基线；高 DPI 由 ImGui 缩放统一处理）——
namespace metrics {
inline constexpr float kTitleBar = 36.0f;
inline constexpr float kMenuBar = 36.0f;
inline constexpr float kFileBar = 44.0f;
inline constexpr float kRail = 92.0f;
// 收起态：只剩图标（工作区可收缩，把宽度还给视口）
inline constexpr float kRailCollapsed = 46.0f;
inline constexpr float kRailButtonH = 62.0f;
// 检查器默认 340：v2.3 内容变多（渲染风格/剖切面/调色盘）后从 300 加宽，
// 但比第一版 380 收窄——给视口留回宽度，宽高比约 1.35:1（默认窗口）。
inline constexpr float kInspector = 420.0f;
inline constexpr float kInspectorMin = 320.0f;
inline constexpr float kInspectorMax = 760.0f;
inline constexpr float kQuickBar = 46.0f;
inline constexpr float kStatusBar = 26.0f;
inline constexpr float kPad = 12.0f;
// 左侧工具栏（v2.17）：夹在左导航与视口之间的一条竖向工具条。
// 56 px = 44 px 按钮 + 左右各 6 px 内边距；收起后宽度归零，视口拿回这 56+8 px。
inline constexpr float kToolShelf = 56.0f;
// 工具栏按钮尺寸（竖排；高度由可用高度自适应，见 Workbench::renderToolShelf）
inline constexpr float kToolButton = 44.0f;
} // namespace metrics

ImU32 col(const ImVec4& c);

// —— 基础控件 ——
// 小节标题（右侧可带弱提示）
void sectionLabel(const char* text, const char* hint = nullptr);

// 卡片：白色圆角容器，必须成对调用。
// 返回 true = 内容可见。**无论返回值如何都必须调用 cardEnd()**：
// 卡片被滚动裁剪时 BeginChild 会返回 false，若此时跳过 cardEnd() 会漏掉
// EndChild + 3 个 style var + 2 个 style color，帧末触发 ImGui 断言。
bool cardBegin(const char* id, const char* title, const char* hint = nullptr);
void cardEnd();

// 左侧导航按钮（选中态：淡蓝底 + 左侧竖条 + 主色文字）
// icon：0 显示 / 1 分子 / 2 表面 / 3 ESP / 4 轨道 / 5 轨迹 / 6 分析 / 7 日志 /
//       8 设置；-1 = 纯文字（无图标）。图标用 ImDrawList 矢量绘制，不依赖字体。
bool navButton(const char* label, bool active, const ImVec2& size, int icon = -1);

// 快捷条胶囊按钮（active = 主色实心）
bool chip(const char* label, bool active, const char* tip = nullptr);

// —— P0 组件（v2.7 组件丰富化：见 docs/_internal/PLAN-UI组件丰富化.md）——
enum class BadgeKind { Neutral, Info, Ok, Warn, Danger };

// 分段控件：2–5 项互斥选择，宽度撑满可用横向空间（替代连排 chip）。
// items 为文字指针数组；idx 越界按 0 处理；返回 true = 选择发生变化。
bool segmented(const char* id, const char* const* items, int n, int* idx);

// 滑块 + 右侧可编辑数值框（框内 Ctrl+点击可键入精确值），返回 true = 值变化。
bool sliderNum(const char* label, float* value, float vmin, float vmax,
               const char* fmt, float step = 0.01f, const char* tip = nullptr);

// 整数步进器：[-] 值 [+]，返回 true = 值变化。
bool stepper(const char* label, int* value, int step, int lo, int hi,
             const char* tip = nullptr);

// 状态徽标（行内绘制，不换行；需要同行时调用方自行 SameLine）
void badge(const char* text, BadgeKind kind = BadgeKind::Neutral);

// 指标胶囊：灰标签 + 亮数值（视口角落 / 状态栏）
void statChip(const char* label, const char* value);

// 视口悬浮工具按钮（小号、半透明底）
bool toolButton(const char* label, const char* tip = nullptr);

// —— 左侧工具栏（v2.17）——
// 图标同样用 ImDrawList 矢量绘制（与 navIcon 同一取舍：不引入图标字体/图片，
// 任意 DPI 都清晰）。新增工具只需在枚举末尾加一项 + 在 tool_icons.cpp 里补一个 case。
enum class ToolIcon : int {
    Pointer = 0,   // 浏览 / 选择（默认相机操作）
    Ruler,         // 测量距离
    TextLabel,     // 文字标注
    Arrow,         // 箭头
    Frame,         // 取景全部
    Reset,         // 重置视角
    Axes,          // 坐标轴方向
    ScaleBar,      // 比例尺与尺寸
    Box,           // 周期水盒
    Hydrogen,      // 隐藏氢原子
    Camera,        // 截图 / 导出
    Sun,           // 光追预览
    Theme,         // 浅色 / 深色
    Trash,         // 清除测量
    Count,
};

// 画一个工具栏图标：center = 图标中心，radius = 半个外框边长（约 10–12 px）。
void toolIcon(ImDrawList* dl, ToolIcon icon, const ImVec2& center, float radius, ImU32 tint);

// 工具栏按钮（竖向方形图标按钮）。
//   active = 选中；toggle = true 时按"开关"样式画（淡主色底 + 主色图标），
//   否则按"互斥模式"样式画（主色实底 + 白图标 + 左侧白条，与 navButton 一致）。
bool shelfButton(ToolIcon icon, bool active, bool toggle, const ImVec2& size,
                 const char* tip = nullptr);

// 表单行：标签在上、控件在下（右检查器宽度有限，横排会挤压控件）
bool sliderRow(const char* label, float* value, float vmin, float vmax,
               const char* fmt, const char* tip = nullptr);
bool toggleRow(const char* label, bool* value, const char* tip = nullptr);
bool comboRow(const char* label, int* index, const char* items,
              const char* tip = nullptr);
bool inputRow(const char* label, char* buf, std::size_t size,
              const char* hint = nullptr);

// 键值行（左灰右亮，状态展示用）
void kvRow(const char* key, const char* value);
void spacer(float h = 8.0f);
void hline();
void helpTip(const char* tip);

// 应用标识（Logo）：透镜环 + 三原子分子，与 app.ico 同一套比例。
// 用矢量图元现画，不依赖任何图片资源/图标字体，因此任意 DPI 都清晰。
//   size   = 正方形外框边长（px），推荐 16–24（标题栏）/ 64+（启动页、关于框）
//   ring   = 透镜环颜色；atom = 原子与键颜色
void logoMark(ImDrawList* dl, const ImVec2& center, float size, ImU32 ring, ImU32 atom);

} // namespace moleculens::app::aurora
