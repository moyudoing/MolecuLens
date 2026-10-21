// =============================================================================
// MolecuLens · engine · export/png/png_encoder.h —— PNG 编码
// 规格来源：主方案 §12.5（离屏 RGBA 回读 → PNG 写出）
//
// 实现：lodepng（单文件、MIT、真实 deflate 压缩）。
//   替代原先自研的"stored-deflate（零压缩）"编码器 —— 后者实测 480×360 就要 691KB；
//   lodepng 自带 deflate 后体积降约一个量级，且原生支持 16-bit（高清导出需要）。
//   选 lodepng 而非 stb_image_write：后者 API 只有 8-bit、源码无 16-bit 分支。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace moleculens::engine {

// RGBA8 像素（行序 top→bottom，每行 width*4 字节）→ PNG 文件。
// 需要恰好 width*height*4 字节；不足则拒绝（避免把越界数据写进图片）。
// 成功返回 true；失败返回 false 并记录 spdlog error。
bool writePng(const std::string& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& rgba);

// RGBA16 像素（每通道 16-bit，主机字节序）→ 16-bit PNG 文件。
// 需要恰好 width*height*4 个采样点；不足则拒绝。
// 用于高动态范围/高位深导出（配合离屏 R16G16B16A16_SFLOAT 附件）。
bool writePng16(const std::string& path, uint32_t width, uint32_t height,
                const std::vector<uint16_t>& rgba16);

// =============================================================================
// 线性 HDR → PNG 采样（16F 半浮点附件回读路径，主方案 §12.5）
// =============================================================================
// 16F 附件是**线性且无硬件 sRGB 编码**的（8-bit SRGB 附件则由硬件编码），
// 故回读后必须在 CPU 侧完成：色调映射 → sRGB 传输 → 量化。
//
// 色调映射：高光肩部压缩（identity 到 knee，其后指数式趋近 1，f 单调不减）。
//   y = x                                    x ≤ k
//   y = k + (1-k)*(1 - exp(-(x-k)/(1-k)))    x > k
// 该式在 x=k 处 C¹ 连续（两侧导数均为 1），数学上上界为 1（x→∞ 才取到），
// 故输入不设上限也**不会硬截断**。
//
// k = 0.85 的选取依据（k 即"开始压缩的亮度"，1-k 决定肩部的输入跨度）：
//   · 肩部输入尺度 = 1-k = 0.15 → 1.0 与 2.0 的线性值映射为 0.945 / 0.9999，
//     **高光层次真正被保留**（16-bit 输出下可分辨到约 2.2 的线性值）
//   · 0.85 以下的常用亮度区间与 8-bit 直通路径逐值一致；0.9 处仅偏暗 0.8%
//     （0.893 vs 0.900），同场景两种位深导出观感基本一致
//   · k 若取到 0.95，肩部尺度只剩 0.05，在 float32/16-bit 下约 1.1 就饱和成纯白
//     —— 等于没做色调映射，故不取
// 注：float32 下 x ≳ 2.5 后 exp 项小于 ULP，y 会饱和到 1.0f（纯白），属浮点精度
//     而非曲线形状问题。
inline constexpr float kTonemapKnee = 0.85f;

float tonemapShoulder(float linear);

// sRGB 传输函数（IEC 61966-2-1）与量化。输入为已色调映射的 [0,1] 线性值。
uint8_t  linearToSrgb8(float mappedLinear);
uint16_t linearToSrgb16(float mappedLinear);

// sRGB 传输函数的逆（linearToSrgb8/16 的反函数）。输入为 sRGB 编码值 [0,1]。
//
// 用途：8-bit SRGB 颜色附件回读后做 SSAA 超采样时，必须先把样本解回**线性**
// 空间再求平均。直接在 sRGB 编码值上平均等价于按 gamma≈2.2 加权 —— 亮暗两侧
// 权重都偏离物理正确的能量平均，抗锯齿后的边缘会系统性偏暗/发灰（同时这也是
// 16F 路径相对 8-bit 路径原本的实质增益，见 writePng16FromHalfRgba 注）。
// 注：alpha 不是 sRGB 编码的（它是线性不透明度），不得经本函数。
float srgbToLinear(float encoded);

// 16F 半浮点回读缓冲（srcWidth×srcHeight，RGBA 半浮点，主机字节序）→
// 盒式降采样到 width×height（srcWidth/srcHeight 必须是 width/height 的整数倍，
// 即 SSAA 倍数）→ 色调映射 + sRGB → **16-bit** PNG。
//
// 注意：求平均在**线性空间**进行（16F 是线性值），比在 8-bit sRGB 值上求平均更
// 正确 —— 这是 16F 路径相对 "8-bit 多采样求平均" 的实质增益。
bool writePng16FromHalfRgba(const std::string& path, uint32_t width, uint32_t height,
                            const std::vector<uint16_t>& halfRgba, uint32_t srcWidth,
                            uint32_t srcHeight);

} // namespace moleculens::engine
