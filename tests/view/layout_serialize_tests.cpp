// =============================================================================
// MolecuLens · tests · view/layout_serialize_tests.cpp —— 布局序列化占位
// 规格来源：主方案 §14.3（布局序列化/恢复自动化；M5 完整落地）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

// 布局序列化格式（M5：base64(imgui_dock_serialized)）；此处验证契约函数
std::string mockSerialize(int leftW, int rightW, int bottomH) {
    return "layout:" + std::to_string(leftW) + ":" + std::to_string(rightW) + ":" +
           std::to_string(bottomH);
}

bool mockDeserialize(const std::string& s, int* leftW, int* rightW, int* bottomH) {
    if (s.rfind("layout:", 0) != 0) {
        return false;
    }
    try {
        const size_t p1 = s.find(':', 7);
        const size_t p2 = s.find(':', p1 + 1);
        *leftW = std::stoi(s.substr(7, p1 - 7));
        *rightW = std::stoi(s.substr(p1 + 1, p2 - p1 - 1));
        *bottomH = std::stoi(s.substr(p2 + 1));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

TEST_CASE("Layout serialize round-trip", "[layout]") {
    const std::string s = mockSerialize(75, 25, 5);
    int l = 0, r = 0, b = 0;
    REQUIRE(mockDeserialize(s, &l, &r, &b));
    REQUIRE(l == 75);
    REQUIRE(r == 25);
    REQUIRE(b == 5);
}

TEST_CASE("Layout deserialize rejects garbage", "[layout]") {
    int l = 0, r = 0, b = 0;
    REQUIRE_FALSE(mockDeserialize("garbage", &l, &r, &b));
    REQUIRE_FALSE(mockDeserialize("layout:abc", &l, &r, &b));
}
