// =============================================================================
// MolecuLens · engine · export/overlay/text_raster_ttf.h —— TTF 文本栅格化（导出叠加用）
// 用途：把 PNG 导出叠加层（ESP 色标条/极值数值等）画成清晰、抗锯齿的文字。
//       实现基于 imgui 包内自带的 stb_truetype 单头（imstb_truetype.h，公共领域），
//       只做 CPU 光栅化，不依赖 ImGui 运行时/Vulkan，CLI 与 GUI 导出共用。
// 字体：默认探测 data/font/OPPO B.ttf 或 可执行目录 font/OPPO_B.ttf（与 GUI 一致），
//       找不到时调用方回退到内置 5x7 位图字体。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace moleculens::engine::text {

// 单个 TTF 字体（PIMPL：头文件不暴露 stb_truetype 类型）
class TtfRenderer {
public:
    TtfRenderer();
    ~TtfRenderer();
    TtfRenderer(const TtfRenderer&) = delete;
    TtfRenderer& operator=(const TtfRenderer&) = delete;

    // 从文件加载字体；失败返回 false（之后所有绘制调用为 no-op）
    bool load(const std::string& fontPath);
    [[nodiscard]] bool loaded() const;

    // 单行 ASCII 文本尺寸（像素）
    [[nodiscard]] float measure(const char* text, int pixelSize) const;
    [[nodiscard]] float lineHeight(int pixelSize) const;

    // 以 (x, baselineY) 为起点绘制一行文本（左上角为 (0,0) 坐标系）。
    // r/g/b/a = 主色；outline 为描边像素宽度（0 = 不描边，>0 用 or/og/ob 描边，
    // 保证深浅背景下都清晰可读）。返回文本 advance 宽度。
    float draw(std::vector<uint8_t>& rgba, int w, int h, float x, float baselineY,
               const char* text, int pixelSize, uint8_t r, uint8_t g, uint8_t b,
               uint8_t a = 255, int outline = 1, uint8_t or_ = 0, uint8_t og_ = 0,
               uint8_t ob_ = 0) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 默认字体路径探测：依次尝试
//   data/font/NotoSansSC-Medium-subset.ttf（当前工作目录，仓库根运行 CLI 时）
//   可执行文件目录/font/NotoSansSC-Medium-subset.ttf（GUI/CLI 构建后拷贝）
//   可执行文件目录/data/font/NotoSansSC-Medium-subset.ttf
// 全部失败返回空串。
std::string findDefaultFontPath();

// 全局默认渲染器（懒加载 findDefaultFontPath()，线程安全）；字体缺失返回 nullptr。
const TtfRenderer* defaultRenderer();

} // namespace moleculens::engine::text
