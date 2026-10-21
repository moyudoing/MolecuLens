// =============================================================================
// MolecuLens · engine · export/png/png_encoder.cpp —— PNG 编码实现（lodepng）
// 格式：8-bit / 16-bit RGBA，真实 deflate 压缩。
//
// 相对此前的自研编码器：把"zlib 封装 + deflate stored（零压缩）"换成 lodepng 的
// 真实 deflate —— 实测 480×360 的图像从 691KB 降到约十分之一量级；并解锁 16-bit。
// 对外签名（writePng）保持不变，调用方无需改动。
// =============================================================================

#include "engine/export/png/png_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <glm/gtc/packing.hpp>   // unpackHalf1x16
#include <lodepng.h>

#include "spdlog/spdlog.h"

namespace moleculens::engine {

float tonemapShoulder(float linear) {
    if (!(linear > 0.0f)) {
        return 0.0f;   // 含 NaN：走 0，避免把 NaN 带进量化
    }
    constexpr float k = kTonemapKnee;
    if (linear <= k) {
        return linear;
    }
    // 肩部：C¹ 连续（两侧导数在 x=k 均为 1）且上界为 1
    return k + (1.0f - k) * (1.0f - std::exp(-(linear - k) / (1.0f - k)));
}

namespace {

// 已映射线性值 → sRGB 传输（IEC 61966-2-1）
inline float srgbTransfer(float c) {
    if (c <= 0.0f) {
        return 0.0f;
    }
    if (c >= 1.0f) {
        return 1.0f;
    }
    return c <= 0.0031308f ? 12.92f * c
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// sRGB 编码值 → 线性（srgbTransfer 的逆；分段点两函数互为反函数）
inline float srgbInverse(float e) {
    if (e <= 0.0f) {
        return 0.0f;
    }
    if (e >= 1.0f) {
        return 1.0f;
    }
    return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

} // namespace

float srgbToLinear(float encoded) { return srgbInverse(encoded); }

uint8_t linearToSrgb8(float mappedLinear) {
    const float s = srgbTransfer(mappedLinear) * 255.0f + 0.5f;
    return static_cast<uint8_t>(std::clamp(s, 0.0f, 255.0f));
}

uint16_t linearToSrgb16(float mappedLinear) {
    const float s = srgbTransfer(mappedLinear) * 65535.0f + 0.5f;
    return static_cast<uint16_t>(std::clamp(s, 0.0f, 65535.0f));
}

bool writePng16FromHalfRgba(const std::string& path, uint32_t width, uint32_t height,
                            const std::vector<uint16_t>& halfRgba, uint32_t srcWidth,
                            uint32_t srcHeight) {
    if (width == 0 || height == 0 || srcWidth == 0 || srcHeight == 0) {
        spdlog::error("[png] writePng16FromHalfRgba: 尺寸非法 {}x{} (src {}x{})", width,
                      height, srcWidth, srcHeight);
        return false;
    }
    if (srcWidth % width != 0 || srcHeight % height != 0) {
        spdlog::error("[png] writePng16FromHalfRgba: 源尺寸 {}x{} 不是目标 {}x{} 的整数倍",
                      srcWidth, srcHeight, width, height);
        return false;
    }
    const size_t need = static_cast<size_t>(srcWidth) * srcHeight * 4u;
    if (halfRgba.size() < need) {
        spdlog::error("[png] writePng16FromHalfRgba: 采样数不足（需 {}，实得 {}）", need,
                      halfRgba.size());
        return false;
    }

    std::vector<uint16_t> out(static_cast<size_t>(width) * height * 4u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            // 盒式降采样：在**线性空间**求平均（16F 是线性值）
            const uint32_t ssx = srcWidth / width;
            const uint32_t ssy = srcHeight / height;
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (uint32_t dy = 0; dy < ssy; ++dy) {
                for (uint32_t dx = 0; dx < ssx; ++dx) {
                    const size_t s =
                        (static_cast<size_t>(y * ssy + dy) * srcWidth + (x * ssx + dx)) * 4u;
                    for (int c = 0; c < 4; ++c) {
                        acc[c] += glm::unpackHalf1x16(halfRgba[s + static_cast<size_t>(c)]);
                    }
                }
            }
            const float inv = 1.0f / static_cast<float>(ssx * ssy);
            const size_t d = (static_cast<size_t>(y) * width + x) * 4u;
            for (int c = 0; c < 3; ++c) {
                out[d + static_cast<size_t>(c)] =
                    linearToSrgb16(tonemapShoulder(acc[c] * inv));
            }
            // alpha 是**线性不透明度**，不做色调映射，直接量化
            out[d + 3] = static_cast<uint16_t>(
                std::clamp(acc[3] * inv, 0.0f, 1.0f) * 65535.0f + 0.5f);
        }
    }
    return writePng16(path, width, height, out);
}

namespace {

bool writeEncoded(const std::string& path, const unsigned char* data, size_t size,
                  const char* what);   // 定义见下（encodeAndWrite 会调用）

// 创建输出文件的父目录。
// 必要性：默认导出路径 export/image.png 的 export/ 通常不存在，ofstream 直接
// 打开失败 → 导出静默失败（历史根因，见 moleculens-fixes）。
bool ensureParentDir(const std::string& path) {
    const std::filesystem::path outPath(path);
    if (!outPath.has_parent_path()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    if (ec) {
        spdlog::error("[export] writePng: 创建输出目录失败 {}",
                      outPath.parent_path().string());
        return false;
    }
    return true;
}

// 按指定的位深编码为 RGBA PNG，并写盘。
//
// ⚠ 必须显式关闭 lodepng 的 auto_convert（默认 LAC_AUTO）：它会在图像**全不透明**
//   时自动丢掉 alpha 通道改写成 RGB、在颜色数少时降低位深 —— 输出格式于是随内容
//   变化。对"调用方要求 RGBA8/RGBA16"的导出语义来说这是不可预期的（同一份代码
//   导出的图，通道数可能不同）。此处锁定格式，保证输出与请求一致。
bool encodeAndWrite(const std::string& path, const unsigned char* image, uint32_t width,
                    uint32_t height, unsigned bitDepth, const char* what) {
    LodePNGState state;
    lodepng_state_init(&state);
    state.info_raw.colortype = LCT_RGBA;
    state.info_raw.bitdepth = bitDepth;
    state.info_png.color.colortype = LCT_RGBA;
    state.info_png.color.bitdepth = bitDepth;
    // 0 = 不自动转换（lodepng 的 LAC_NO）。本版本头文件只声明 `unsigned auto_convert`
    // 而未导出 LAC_* 常量名，故用其文档值 0；开启时编码器会忽略 info_png.color
    // 自行选择颜色模式，使输出格式随图像内容变化。
    state.encoder.auto_convert = 0;

    unsigned char* out = nullptr;
    size_t outSize = 0;
    const unsigned err = lodepng_encode(&out, &outSize, image, width, height, &state);
    lodepng_state_cleanup(&state);
    if (err != 0) {
        spdlog::error("[export] {}: 编码失败: {}", what, lodepng_error_text(err));
        std::free(out);
        return false;
    }
    const bool ok = writeEncoded(path, out, outSize, what);
    std::free(out);
    return ok;
}

// lodepng 的 encode-memory 输出由 malloc 分配，写盘后需 free
bool writeEncoded(const std::string& path, const unsigned char* data, size_t size,
                  const char* what) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        spdlog::error("[export] {}: 无法打开输出文件 {}", what, path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
        spdlog::error("[export] {}: 写入失败 {}", what, path);
        return false;
    }
    return true;
}

} // namespace

bool writePng(const std::string& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& rgba) {
    if (width == 0 || height == 0) {
        spdlog::error("[export] writePng: 非法尺寸 {}x{}", width, height);
        return false;
    }
    const size_t need = static_cast<size_t>(width) * height * 4u;
    if (rgba.size() < need) {
        // 严格校验：数据不足时拒绝，而不是把未初始化内存写进图片
        spdlog::error("[export] writePng: 尺寸/像素数据不匹配 ({}x{} need={} have={})",
                      width, height, need, rgba.size());
        return false;
    }
    if (!ensureParentDir(path)) {
        return false;
    }

    // 传裸指针而非 vector：大分辨率下复制一份代价高（8K RGBA8 约 132 MB）
    const bool ok = encodeAndWrite(path, rgba.data(), width, height, 8, "writePng");
    if (ok) {
        spdlog::info("[export] PNG 导出: {} ({}x{} 8-bit RGBA)", path, width, height);
    }
    return ok;
}

bool writePng16(const std::string& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba16) {
    if (width == 0 || height == 0) {
        spdlog::error("[export] writePng16: 非法尺寸 {}x{}", width, height);
        return false;
    }
    const size_t samples = static_cast<size_t>(width) * height * 4u;
    if (rgba16.size() < samples) {
        spdlog::error("[export] writePng16: 尺寸/采样数据不匹配 ({}x{} need={} have={})",
                      width, height, samples, rgba16.size());
        return false;
    }
    if (!ensureParentDir(path)) {
        return false;
    }

    // lodepng 的 16-bit 原始数据按**大端**解释；不可直接 reinterpret_cast 主机序
    // 的 uint16 数组（小端平台上字节序会整体颠倒）。此处显式转换。
    std::vector<unsigned char> be(samples * 2u);
    for (size_t i = 0; i < samples; ++i) {
        be[i * 2u] = static_cast<unsigned char>(rgba16[i] >> 8);
        be[i * 2u + 1u] = static_cast<unsigned char>(rgba16[i] & 0xFFu);
    }

    const bool ok = encodeAndWrite(path, be.data(), width, height, 16, "writePng16");
    if (ok) {
        spdlog::info("[export] PNG 导出: {} ({}x{} 16-bit RGBA)", path, width, height);
    }
    return ok;
}

} // namespace moleculens::engine
