// =============================================================================
// MolecuLens · engine · export/overlay/text_raster_ttf.cpp —— TTF 文本栅格化实现
// 依赖：imstb_truetype.h（vcpkg imgui 包自带，公共领域单头；仅作 CPU 光栅化，
//       不链接 ImGui 运行时）。字体默认 Noto Sans SC 子集，与 GUI 字体一致。
// =============================================================================

#include "engine/export/overlay/text_raster_ttf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

namespace moleculens::engine::text {

namespace {

// 读整个文件到内存（>0 字节才认为成功）
bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return read == out.size();
}

// 可执行文件所在目录（Windows 下用 GetModuleFileName；其它平台回退当前目录）
std::string executableDirectory() {
#if defined(_WIN32)
    char buffer[4096] = {};
    const unsigned long len = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    if (len > 0 && len < sizeof(buffer)) {
        std::error_code ec;
        const auto dir = std::filesystem::path(std::string(buffer, len)).parent_path();
        return dir.string();
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
}

// 在缓冲区里混合一个带 alpha 的像素（源色固定，alpha=bitmap 值 × 传入 alpha）
inline void blendPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return;
    }
    const std::size_t o = (static_cast<std::size_t>(y) * w + x) * 4;
    const float fa = static_cast<float>(a) / 255.0f;
    rgba[o + 0] = static_cast<uint8_t>(rgba[o + 0] * (1.0f - fa) + r * fa + 0.5f);
    rgba[o + 1] = static_cast<uint8_t>(rgba[o + 1] * (1.0f - fa) + g * fa + 0.5f);
    rgba[o + 2] = static_cast<uint8_t>(rgba[o + 2] * (1.0f - fa) + b * fa + 0.5f);
}

} // namespace

// ---------------------------------------------------------------------------
// TtfRenderer
// ---------------------------------------------------------------------------

struct TtfRenderer::Impl {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool loaded = false;
    int ascent = 0;    // 字体单位
    int descent = 0;
    int lineGap = 0;
};

TtfRenderer::TtfRenderer() : impl_(std::make_unique<Impl>()) {}
TtfRenderer::~TtfRenderer() = default;

bool TtfRenderer::load(const std::string& fontPath) {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->loaded = false;
    if (!readFileBytes(fontPath, impl_->data)) {
        return false;
    }
    const int offset = stbtt_GetFontOffsetForIndex(impl_->data.data(), 0);
    if (offset < 0 || stbtt_InitFont(&impl_->info, impl_->data.data(), offset) == 0) {
        return false;
    }
    stbtt_GetFontVMetrics(&impl_->info, &impl_->ascent, &impl_->descent, &impl_->lineGap);
    impl_->loaded = true;
    return true;
}

bool TtfRenderer::loaded() const { return impl_ != nullptr && impl_->loaded; }

float TtfRenderer::measure(const char* text, int pixelSize) const {
    if (!loaded() || text == nullptr || pixelSize <= 0) {
        return 0.0f;
    }
    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<float>(pixelSize));
    float x = 0.0f;
    int prev = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const int cp = static_cast<unsigned char>(*p);
        int advance = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&impl_->info, cp, &advance, &lsb);
        if (prev != 0) {
            x += static_cast<float>(stbtt_GetCodepointKernAdvance(&impl_->info, prev, cp)) *
                 scale;
        }
        x += static_cast<float>(advance) * scale;
        prev = cp;
    }
    return x;
}

float TtfRenderer::lineHeight(int pixelSize) const {
    if (!loaded() || pixelSize <= 0) {
        return 0.0f;
    }
    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<float>(pixelSize));
    return static_cast<float>(impl_->ascent - impl_->descent + impl_->lineGap) * scale;
}

float TtfRenderer::draw(std::vector<uint8_t>& rgba, int w, int h, float x, float baselineY,
                        const char* text, int pixelSize, uint8_t r, uint8_t g, uint8_t b,
                        uint8_t a, int outline, uint8_t or_, uint8_t og_,
                        uint8_t ob_) const {
    if (!loaded() || text == nullptr || pixelSize <= 0 ||
        rgba.size() < static_cast<std::size_t>(w) * h * 4) {
        return 0.0f;
    }
    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<float>(pixelSize));
    const float advanceWidth =
        measure(text, pixelSize);   // 先算 advance（绘制后再算会重复 kerning 逻辑）

    // 描边：把整行文本用深色在 8 邻域错位画一遍（1px），再画主色。
    // 目的：无论深浅背景都清晰可读（参考图的标签也有明显描边）。
    const auto drawOnce = [&](float ox, float oy, uint8_t cr, uint8_t cg, uint8_t cb,
                              uint8_t ca) {
        float penX = x + ox;
        const float baseline = baselineY + oy;
        int prev = 0;
        for (const char* p = text; *p != '\0'; ++p) {
            const int cp = static_cast<unsigned char>(*p);
            int advance = 0;
            int lsb = 0;
            stbtt_GetCodepointHMetrics(&impl_->info, cp, &advance, &lsb);
            if (prev != 0) {
                penX += static_cast<float>(
                            stbtt_GetCodepointKernAdvance(&impl_->info, prev, cp)) *
                        scale;
            }
            int bx = 0;
            int by = 0;
            int bw = 0;
            int bh = 0;
            int xoff = 0;
            int yoff = 0;
            unsigned char* bitmap = stbtt_GetCodepointBitmap(
                &impl_->info, scale, scale, cp, &bw, &bh, &xoff, &yoff);
            if (bitmap != nullptr) {
                bx = static_cast<int>(std::lround(penX)) + xoff;
                by = static_cast<int>(std::lround(baseline)) + yoff;
                for (int row = 0; row < bh; ++row) {
                    for (int col = 0; col < bw; ++col) {
                        const uint8_t alpha = bitmap[row * bw + col];
                        if (alpha == 0) {
                            continue;
                        }
                        const uint8_t effective = static_cast<uint8_t>(
                            (static_cast<unsigned>(alpha) * ca + 127u) / 255u);
                        blendPixel(rgba, w, h, bx + col, by + row, cr, cg, cb, effective);
                    }
                }
                stbtt_FreeBitmap(bitmap, nullptr);
            }
            penX += static_cast<float>(advance) * scale;
            prev = cp;
        }
    };

    if (outline > 0) {
        const uint8_t oa = static_cast<uint8_t>(std::min<int>(a, 210));
        for (int oy = -outline; oy <= outline; ++oy) {
            for (int ox = -outline; ox <= outline; ++ox) {
                if (ox == 0 && oy == 0) {
                    continue;
                }
                drawOnce(static_cast<float>(ox), static_cast<float>(oy), or_, og_, ob_,
                         oa);
            }
        }
    }
    drawOnce(0.0f, 0.0f, r, g, b, a);
    return advanceWidth;
}

// ---------------------------------------------------------------------------
// 默认字体探测 / 全局渲染器
// ---------------------------------------------------------------------------

std::string findDefaultFontPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    const fs::path exeDir = executableDirectory();

    const std::vector<fs::path> candidates = {
        cwd / "data" / "font" / "NotoSansSC-Medium-subset.ttf",
        exeDir / "font" / "NotoSansSC-Medium-subset.ttf",
        exeDir / "data" / "font" / "NotoSansSC-Medium-subset.ttf",
    };
    for (const fs::path& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
    }
    return std::string();
}

const TtfRenderer* defaultRenderer() {
    static std::once_flag once;
    static std::unique_ptr<TtfRenderer> renderer;
    std::call_once(once, []() {
        const std::string path = findDefaultFontPath();
        if (path.empty()) {
            return;
        }
        auto candidate = std::make_unique<TtfRenderer>();
        if (candidate->load(path)) {
            renderer = std::move(candidate);
        }
    });
    return renderer.get();
}

} // namespace moleculens::engine::text
