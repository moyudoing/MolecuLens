// =============================================================================
// MolecuLens · app · view/dock/base64.h —— base64 编解码（头文件实现）
// 规格来源：主方案 §12.2（布局以 base64(imgui ini) 存入 settings.json）
//
// 为什么单独成头 + 内联实现：
//   base64 是纯函数、无状态，**不依赖 ImGui**；而布局序列化的其余部分（SaveIni/
//   LoadIni、DockBuilder）必须在 ImGui 上下文里跑。把它独立出来，测试可以不链接
//   app 层（moleculens_app 是可执行文件，测试引擎无法链接）就覆盖真实的编解码 ——
//   此前 tests/view/layout_serialize_tests.cpp 只能测两个与生产代码无关的局部
//   mock，等于没有覆盖。
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace moleculens::app::dock {

inline constexpr const char* kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64Encode(const std::string& raw) {
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    for (size_t i = 0; i < raw.size(); i += 3) {
        const uint32_t a = static_cast<uint8_t>(raw[i]);
        const uint32_t b = i + 1 < raw.size() ? static_cast<uint8_t>(raw[i + 1]) : 0;
        const uint32_t c = i + 2 < raw.size() ? static_cast<uint8_t>(raw[i + 2]) : 0;
        const uint32_t n = (a << 16) | (b << 8) | c;
        out += kBase64Alphabet[(n >> 18) & 63];
        out += kBase64Alphabet[(n >> 12) & 63];
        out += i + 1 < raw.size() ? kBase64Alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? kBase64Alphabet[n & 63] : '=';
    }
    return out;
}

// 解码失败返回 false（非法字符 / 无有效字节）。'=' 与换行/回车被忽略。
inline bool base64Decode(const std::string& base64, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    static const std::array<int8_t, 256> kRev = [] {
        std::array<int8_t, 256> table{};
        table.fill(-1);
        for (int i = 0; i < 64; ++i) {
            table[static_cast<uint8_t>(kBase64Alphabet[i])] = static_cast<int8_t>(i);
        }
        return table;
    }();

    std::string raw;
    raw.reserve(base64.size() / 4 * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (char ch : base64) {
        if (ch == '=' || ch == '\n' || ch == '\r') {
            continue;
        }
        const int8_t v = kRev[static_cast<uint8_t>(ch)];
        if (v < 0) {
            return false;
        }
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    if (raw.empty()) {
        return false;
    }
    *out = std::move(raw);
    return true;
}

} // namespace moleculens::app::dock
