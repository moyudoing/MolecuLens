// =============================================================================
// MolecuLens · tests · engine/export/png_encoder_tests.cpp —— PNG 编码器测试
// 校验：签名 / IHDR 尺寸与位深 / 颜色类型恒为 RGBA / IDAT+IEND / 压缩生效 /
//       16-bit 往返 / 尺寸不匹配拒绝
//
// 编码器已改用 lodepng（此前自研版是 stored-deflate 零压缩，480×360 就要 691KB）。
// 故这里除了结构校验，还显式断言"真实压缩生效"与"输出颜色类型不随内容变化"
// —— 后者是 lodepng 默认 auto_convert 会踩的坑（全不透明时悄悄改写成 RGB）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <glm/gtc/packing.hpp>   // packHalf1x16（构造 16F 回读缓冲）
#include <lodepng.h>

#include "engine/export/png/png_encoder.h"

using namespace moleculens::engine;

namespace {

uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

} // namespace

TEST_CASE("writePng emits a structurally valid PNG", "[export][png]") {
    const uint32_t w = 2, h = 2;
    // 2x2 RGBA：红 / 绿 / 蓝 / 白
    std::vector<uint8_t> rgba = {
        255, 0,   0,   255,  0,   255, 0,   255,
        0,   0,   255, 255,  255, 255, 255, 255,
    };
    const std::string path = "test_export.png";
    REQUIRE(writePng(path, w, h, rgba));

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    in.close();
    std::remove(path.c_str());

    // 签名
    REQUIRE(data.size() >= 8);
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    REQUIRE(std::memcmp(data.data(), sig, 8) == 0);

    // IHDR：type + 13 字节 data + CRC
    REQUIRE(data.size() >= 8 + 4 + 4 + 13 + 4);
    REQUIRE(std::memcmp(data.data() + 8 + 4, "IHDR", 4) == 0);
    const uint8_t* ihdr = data.data() + 8 + 4 + 4;
    REQUIRE(readBE32(ihdr) == w);
    REQUIRE(readBE32(ihdr + 4) == h);
    REQUIRE(ihdr[8] == 8);   // bitDepth
    REQUIRE(ihdr[9] == 6);   // colorType RGBA

    // 存在 IDAT 与 IEND 块（扫描全部块）
    bool hasIdat = false, hasIend = false;
    size_t pos = 8;
    while (pos + 12 <= data.size()) {
        const uint32_t len = readBE32(data.data() + pos);
        const char* type = reinterpret_cast<const char*>(data.data() + pos + 4);
        if (std::strncmp(type, "IDAT", 4) == 0) {
            hasIdat = true;
        }
        if (std::strncmp(type, "IEND", 4) == 0) {
            hasIend = true;
        }
        pos += 12 + len;
        if (pos > data.size()) {
            break;
        }
    }
    REQUIRE(hasIdat);
    REQUIRE(hasIend);
}

TEST_CASE("writePng rejects size/data mismatch", "[export][png]") {
    const std::vector<uint8_t> tiny(4);   // 仅 1 像素
    REQUIRE_FALSE(writePng("bad.png", 100, 100, tiny));
}

TEST_CASE("writePng actually compresses", "[export][png]") {
    // 平滑渐变：stored-deflate（旧实现）会写出 ≈ w*h*4 字节，真实 deflate 应远小于此。
    // 这条断言直接看住"压缩是否真的生效"，而不是只校验文件结构合法。
    const uint32_t w = 256, h = 256;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            rgba[i + 0] = static_cast<uint8_t>(x);
            rgba[i + 1] = static_cast<uint8_t>(y);
            rgba[i + 2] = 128;
            rgba[i + 3] = 255;
        }
    }
    const std::string path = "test_compress.png";
    REQUIRE(writePng(path, w, h, rgba));
    const auto fileSize = std::filesystem::file_size(path);
    std::filesystem::remove(path);
    const size_t rawSize = static_cast<size_t>(w) * h * 4u;
    REQUIRE(fileSize < rawSize / 4u);
}

TEST_CASE("writePng keeps RGBA color type regardless of content", "[export][png]") {
    // lodepng 默认 auto_convert 会在图像全不透明时丢掉 alpha 改写成 RGB。
    // 导出语义要求"请求什么格式就写什么格式"，故此处对不透明与含透明两种输入
    // 都断言 colorType == 6。
    const auto colorTypeOf = [](const std::string& path) -> int {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        if (d.size() < 26) {
            return -1;
        }
        return d[25];   // sig(8) + len(4) + "IHDR"(4) + w(4) + h(4) + bitDepth(1) → colorType
    };

    const uint32_t w = 2, h = 1;
    const std::vector<uint8_t> opaque = {255, 0, 0, 255, 0, 255, 0, 255};
    REQUIRE(writePng("test_opaque.png", w, h, opaque));
    REQUIRE(colorTypeOf("test_opaque.png") == 6);
    std::filesystem::remove("test_opaque.png");

    const std::vector<uint8_t> translucent = {255, 0, 0, 0, 0, 255, 0, 128};
    REQUIRE(writePng("test_trans.png", w, h, translucent));
    REQUIRE(colorTypeOf("test_trans.png") == 6);
    std::filesystem::remove("test_trans.png");
}

TEST_CASE("writePng16 round-trips 16-bit samples", "[export][png]") {
    const uint32_t w = 4, h = 3;
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4u);
    for (size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<uint16_t>(i * 4001u + 137u);
    }
    const std::string path = "test16.png";
    REQUIRE(writePng16(path, w, h, px));

    // 结构：bitDepth = 16 且 colorType = 6
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        REQUIRE(d.size() > 26);
        REQUIRE(d[24] == 16);   // bitDepth
        REQUIRE(d[25] == 6);    // colorType RGBA
    }

    // 用 lodepng 解码比对：验证"16-bit 采样按大端写入"的假设成立
    // （lodepng 解码输出同样是 16-bit 大端字节流）
    std::vector<unsigned char> raw;
    unsigned uw = 0, uh = 0;
    const unsigned err = lodepng::decode(raw, uw, uh, path, LCT_RGBA, 16);
    REQUIRE(err == 0);
    REQUIRE(uw == w);
    REQUIRE(uh == h);
    REQUIRE(raw.size() == px.size() * 2u);
    for (size_t i = 0; i < px.size(); ++i) {
        const uint16_t be = static_cast<uint16_t>((raw[i * 2u] << 8) | raw[i * 2u + 1u]);
        REQUIRE(be == px[i]);
    }
    std::filesystem::remove(path);
}

TEST_CASE("writePng16 rejects size/data mismatch", "[export][png]") {
    const std::vector<uint16_t> tiny(4);
    REQUIRE_FALSE(writePng16("bad16.png", 100, 100, tiny));
}

// =============================================================================
// 线性 HDR → PNG 采样（16F 半浮点附件回读路径）
// =============================================================================

TEST_CASE("shoulder tonemap is identity below the knee and bounded above",
          "[export][png]") {
    // knee 以下逐值直通：保证常用亮度区间与 8-bit 直通路径一致（同场景两种位深
    // 导出观感不应不同）
    REQUIRE(tonemapShoulder(0.0f) == 0.0f);
    REQUIRE(tonemapShoulder(0.25f) == 0.25f);
    REQUIRE(tonemapShoulder(kTonemapKnee) == kTonemapKnee);
    // 负值与 NaN → 0（不把非法值带进量化）
    REQUIRE(tonemapShoulder(-1.0f) == 0.0f);
    REQUIRE(tonemapShoulder(std::nanf("")) == 0.0f);

    // knee 之上非递减且上界为 1（数学上永不截断）。
    // float32 下 x ≳ 2.5 后 exp 项小于 ULP，y 饱和到**恰好 1.0f** —— 浮点精度
    // 而非曲线形状问题。故：全局断言非递减且 y ≤ 1，在可分辨区间断言严格递增。
    float prev = kTonemapKnee;
    for (float x = kTonemapKnee + 0.01f; x < 20.0f; x += 0.01f) {
        const float y = tonemapShoulder(x);
        REQUIRE(y >= prev);
        REQUIRE(y <= 1.0f);
        if (x <= 2.0f) {
            REQUIRE(y > prev);   // 可分辨区间内严格递增
        }
        prev = y;
    }
    // 高光层次**确实被保留**：1.0 / 1.5 / 2.0 三个线性值映射到互不相同的输出，
    // 且 2.0 仍严格小于纯白 —— 这是"色调映射"相对"直接 clamp"的全部意义所在。
    const float t10 = tonemapShoulder(1.0f);
    const float t15 = tonemapShoulder(1.5f);
    const float t20 = tonemapShoulder(2.0f);
    REQUIRE(t10 > kTonemapKnee);
    REQUIRE(t15 > t10);
    REQUIRE(t20 > t15);
    REQUIRE(t20 < 1.0f);
    // 16-bit 量化后仍可分辨（差值需 > 1 个 16-bit LSB = 1/65535）
    REQUIRE((t15 - t10) * 65535.0f > 1.0f);
    REQUIRE((t20 - t15) * 65535.0f > 1.0f);

    // knee 处连续（两侧都等于 knee）
    REQUIRE(std::fabs(tonemapShoulder(kTonemapKnee - 1e-4f) - kTonemapKnee) < 1e-3f);
    REQUIRE(std::fabs(tonemapShoulder(kTonemapKnee + 1e-4f) - kTonemapKnee) < 1e-3f);
}

TEST_CASE("sRGB transfer matches the IEC curve", "[export][png]") {
    REQUIRE(linearToSrgb8(0.0f) == 0);
    REQUIRE(linearToSrgb8(1.0f) == 255);
    REQUIRE(linearToSrgb16(0.0f) == 0);
    REQUIRE(linearToSrgb16(1.0f) == 65535);
    // 线性 0.5 → sRGB ≈ 0.7354 → 188/255（IEC 61966-2-1 的已知取值）
    REQUIRE(linearToSrgb8(0.5f) == 188);
    // 线性段（c ≤ 0.0031308 时斜率 12.92）：12.92*c*255 + 0.5 后截断
    REQUIRE(linearToSrgb8(0.001f) == 3);          // 12.92*0.001*255 = 3.29 → 3
    REQUIRE(linearToSrgb8(0.002f) == 7);          // 12.92*0.002*255 = 6.59 → 7
    // 越界夹紧
    REQUIRE(linearToSrgb8(2.0f) == 255);
    REQUIRE(linearToSrgb16(-1.0f) == 0);
}

TEST_CASE("writePng16FromHalfRgba validates sizes and writes 16-bit", "[export][png]") {
    // 源尺寸必须是目标尺寸的整数倍（SSAA）；否则拒绝
    std::vector<uint16_t> half(static_cast<size_t>(6) * 4u * 4u, 0);
    REQUIRE_FALSE(writePng16FromHalfRgba("bad_half.png", 4, 3, half, 6, 4));
    // 采样数不足 → 拒绝
    REQUIRE_FALSE(writePng16FromHalfRgba("bad_half.png", 2, 2,
                                        std::vector<uint16_t>(4), 4, 4));

    // 2×2 源 → 1×1 目标（SSAA=2）：四个样本线性值各不相同，取平均后必须落在
    // 两者之间（验证"在线性空间求平均"而非取某一个样本）
    const uint16_t h001 = glm::packHalf1x16(0.0f);
    const uint16_t h010 = glm::packHalf1x16(0.1f);
    const uint16_t h020 = glm::packHalf1x16(0.2f);
    const uint16_t h030 = glm::packHalf1x16(0.3f);
    const uint16_t one = glm::packHalf1x16(1.0f);
    // 顺序：左上(0.0) 右上(0.1) 左下(0.2) 右下(0.3)
    const std::vector<uint16_t> src = {
        h001, h001, h001, one,   // px(0,0)
        h010, h010, h010, one,   // px(1,0)
        h020, h020, h020, one,   // px(0,1)
        h030, h030, h030, one,   // px(1,1)
    };
    const std::string path = "test_half.png";
    REQUIRE(writePng16FromHalfRgba(path, 1, 1, src, 2, 2));

    // 解码比对。理想值是 0.15 线性 → linearToSrgb16(tonemapShoulder(0.15))；
    // 但半浮点无法精确表示 0.1/0.2/0.3，解码后求平均得到的是 0.14999390 而非
    // 0.15，量化后相差 1 个 LSB → 用容差断言（同时验证"在线性空间求平均"：
    // 若误在 sRGB 空间平均，结果会偏低约 4500 个 LSB，远超此容差）。
    const uint16_t ideal = linearToSrgb16(tonemapShoulder(0.15f));
    std::vector<unsigned char> raw;
    unsigned uw = 0, uh = 0;
    REQUIRE(lodepng::decode(raw, uw, uh, path, LCT_RGBA, 16) == 0);
    REQUIRE(uw == 1u);
    REQUIRE(uh == 1u);
    REQUIRE(raw.size() == 8u);
    const uint16_t got = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    REQUIRE(std::abs(static_cast<int>(got) - static_cast<int>(ideal)) <= 4);
    // 且必须落在最暗与最亮样本之间（证明确实做了平均，而不是取了某个样本）
    const uint16_t lo = linearToSrgb16(tonemapShoulder(0.0f));
    const uint16_t hi = linearToSrgb16(tonemapShoulder(0.3000488281f));
    REQUIRE(got >= lo);
    REQUIRE(got <= hi);
    std::filesystem::remove(path);
}
