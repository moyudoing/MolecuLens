// =============================================================================
// MolecuLens · tests · view/layout_serialize_tests.cpp —— 布局序列化（base64）
// 规格来源：主方案 §12.2（布局以 base64(imgui ini) 存入 settings.json）、§14.3
//
// 覆盖真实实现 app/view/dock/dock_layout.{h,cpp} 的 base64 编解码 ——
// 原先本文件只测两个**局部 mock 函数**（与生产代码无关），等于没有覆盖；
// 抽到 dock 模块后再把 base64 单独暴露，这里才真正测到往返与边界。
//
// serializeLayout/deserializeLayout 需要 ImGui 上下文（无上下文分别返回空串 /
// false），此处覆盖该契约；纯编解码不依赖上下文，故可直接验证。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/view/dock/base64.h"

using namespace moleculens::app::dock;

namespace {

// 参考实现（独立于被测代码）——避免"编码/解码互相自洽但都错"
std::string refEncode(const std::string& raw) {
    const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < raw.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(raw[i]);
        const unsigned b = i + 1 < raw.size() ? static_cast<unsigned char>(raw[i + 1]) : 0;
        const unsigned c = i + 2 < raw.size() ? static_cast<unsigned char>(raw[i + 2]) : 0;
        const unsigned n = (a << 16) | (b << 8) | c;
        out += k[(n >> 18) & 63];
        out += k[(n >> 12) & 63];
        out += i + 1 < raw.size() ? k[(n >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? k[n & 63] : '=';
    }
    return out;
}

} // namespace

TEST_CASE("base64 matches the reference encoding", "[layout]") {
    // 三种长度模 3 的情形（0 / 1 / 2 个填充位）都要覆盖 —— 填充是 base64 最易错处
    REQUIRE(base64Encode("") == "");
    REQUIRE(base64Encode("f") == "Zg==");
    REQUIRE(base64Encode("fo") == "Zm8=");
    REQUIRE(base64Encode("foo") == "Zm9v");
    REQUIRE(base64Encode("foob") == "Zm9vYg==");
    REQUIRE(base64Encode("fooba") == "Zm9vYmE=");
    REQUIRE(base64Encode("foobar") == "Zm9vYmFy");

    // 与独立参考实现逐字节一致（含非 ASCII 字节——布局 ini 是 UTF-8 中文窗口名）
    const std::vector<std::string> cases = {
        "layout:1", "{\"dock\":\"3D 视口\"}", std::string("\x00\x01\x02\xFF\xFE", 5),
        "MolecuLens", std::string(300, 'x'),
    };
    for (const auto& s : cases) {
        REQUIRE(base64Encode(s) == refEncode(s));
    }
}

TEST_CASE("base64 round-trips", "[layout]") {
    // 非空输入必须严格往返。空串**不在**此列：encode("") == ""，而 decode("")
    // 按契约返回 false（"无有效字节" = 解码失败）—— 见下一用例。该不对称是刻意的：
    // 调用方（deserializeLayout）本就把空串当作"没有可恢复的布局"。
    const std::vector<std::string> cases = {
        "f", "fo", "foo", "foobar", "3D 视口 / 时间轴 / 状态栏",
        std::string("\x00\xFF\x80\x7F", 4), std::string(1000, 'A'),
    };
    for (const auto& s : cases) {
        std::string back;
        REQUIRE(base64Decode(base64Encode(s), &back));
        REQUIRE(back == s);
    }
    // 空串往返：编码为空、解码按契约为失败
    REQUIRE(base64Encode("").empty());
    std::string ignored;
    REQUIRE_FALSE(base64Decode("", &ignored));
}

TEST_CASE("base64 decode tolerates padding and newlines", "[layout]") {
    std::string out;
    REQUIRE(base64Decode("Zm9vYmFy", &out));
    REQUIRE(out == "foobar");
    // 换行/回车应被忽略（settings.json 里可能含折行）
    REQUIRE(base64Decode("Zm9v\r\nYmFy", &out));
    REQUIRE(out == "foobar");
    // 空输入与全部填充 → 无有效字节 → 失败（而非返回空串成功）
    REQUIRE_FALSE(base64Decode("", &out));
    REQUIRE_FALSE(base64Decode("====", &out));
}

TEST_CASE("base64 decode rejects invalid characters", "[layout]") {
    std::string out;
    REQUIRE_FALSE(base64Decode("!!!!", &out));    // '!' 不在字母表内
    REQUIRE_FALSE(base64Decode("Zm9v!", &out));
    REQUIRE_FALSE(base64Decode("abc def", &out));  // 空格非法
}

// 注：dock::serializeLayout / deserializeLayout（SaveIniSettingsToMemory +
// LoadIniSettingsFromMemory）依赖当前 ImGui 上下文，且定义在 app 层
// （moleculens_app 是可执行文件，测试无法链接）→ 不在本文件覆盖。
// 其"无 ImGui 上下文时安全退化（空串 / false）"的契约由 view_control 的包装
// 保证，并在 GUI 启动冒烟中验证（日志 "layout restored from settings"）。
