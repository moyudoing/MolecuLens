// =============================================================================
// MolecuLens · engine · export/overlay/annotation/scene_overlay.h —— 场景标注数据
//
// 用途：把「视图叠加层」的标注（水盒/标签/箭头/水印）变成一份与 ImGui 无关的
// 纯数据快照，供两条链路共用：
//   · 视口实时绘制（app/workbench/workbench.cpp::drawSceneOverlay，ImGui）
//   · PNG 导出栅格化（engine/export/overlay/annotation/scene_overlay_raster.h）
//
// 为什么放 engine 而不是 app：
//   MolScene::exportToPng 在 app/view 里，但导出栅格器要能在无 ImGui 的上下文
//   （CLI/离屏/单测）工作；数据快照放 engine 后，导出侧不需要认识 Workbench。
//
// 快照是**进程级单例**（与 engine::espOverlaySnapshot 同一取舍）：每帧由
// Workbench 覆盖写入，导出时读取。它只存"画什么"，不存任何 UI 状态。
// =============================================================================

#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::engine::scene_overlay {

// 箭头样式：普通直线箭头 / 波浪（光子「光」箭头，参考 TCBG 示意图）
enum class ArrowStyle : int {
    Plain = 0,
    Zigzag = 1,
};

// 相对图像的锚点（水印用）
enum class Anchor : int {
    BottomRight = 0,
    BottomLeft,
    Center,
    TopRight,
};

// —— 水盒 / 周期盒：用 3 个棱向量表示（正交盒 = 对角向量；三斜盒照搬 9 个分量）——
// origin = 一个角的坐标；a1/a2/a3 = 从该角出发的三条棱。12 条棱 = origin + 组合。
struct Box {
    bool visible = false;
    glm::vec3 origin{0.0f};
    glm::vec3 a1{20.0f, 0.0f, 0.0f};
    glm::vec3 a2{0.0f, 20.0f, 0.0f};
    glm::vec3 a3{0.0f, 0.0f, 20.0f};
    float color[4] = {0.45f, 0.62f, 0.85f, 0.9f};
    float lineWidth = 1.4f;   // 视口像素（导出时按分辨率等比放大）
};

// —— 文字标签：锚在世界坐标，投影到屏幕后画字（可选引导点）——
struct Label {
    std::string text;
    glm::vec3 pos{0.0f};
    float color[4] = {1.0f, 0.92f, 0.55f, 1.0f};
    float pixelSize = 16.0f;   // 视口像素；导出按分辨率等比放大
    bool leader = true;        // 是否在锚点画小圆点
    bool visible = true;
};

// —— 箭头：世界坐标两端；宽度/箭头大小/双头/波浪 ——
struct Arrow {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    float color[4] = {1.0f, 0.85f, 0.30f, 1.0f};
    float width = 2.2f;        // 线宽（视口像素）
    float headScale = 1.0f;    // 箭头大小倍率
    bool doubleHead = false;   // 两端都有箭头
    ArrowStyle style = ArrowStyle::Plain;
    bool visible = true;
};

// —— 水印：图角落的大字（参考图的 TCBG 底纹）——
struct Watermark {
    bool visible = false;
    std::string text;
    float color[4] = {1.0f, 1.0f, 1.0f, 0.16f};
    float scale = 0.10f;       // 字号 = 图像高度 × scale
    Anchor anchor = Anchor::BottomRight;
};

struct Snapshot {
    Box box;
    std::vector<Label> labels;
    std::vector<Arrow> arrows;
    Watermark watermark;

    [[nodiscard]] bool empty() const {
        return !box.visible && labels.empty() && arrows.empty() && !watermark.visible;
    }
};

// 进程级快照（每帧由 Workbench 覆盖写；导出侧只读拷贝）。
// inline 函数的静态局部变量在 C++17 下跨 TU 唯一 —— 无需额外 .cpp。
inline Snapshot& mutableSnapshot() {
    static Snapshot snapshot;
    return snapshot;
}

inline Snapshot snapshot() {
    return mutableSnapshot();
}

} // namespace moleculens::engine::scene_overlay
