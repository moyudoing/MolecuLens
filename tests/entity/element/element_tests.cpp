// =============================================================================
// MolecuLens · tests · entity/element/element_tests.cpp —— 元素表测试
// 规格来源：主方案 §4.1 实现要求（118 个元素完整、颜色为合法 sRGB）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "entity/element/element.h"

using namespace moleculens::entity;

TEST_CASE("Element table has all 118 elements", "[element]") {
    REQUIRE(kElementTable.size() == 119);   // 0 = Unknown 占位
    // 1..118 全部非空
    for (uint8_t i = 1; i <= 118; ++i) {
        const auto& info = kElementTable[i];
        REQUIRE(info.atomicNumber == i);
        REQUIRE(info.symbol[0] != '\0');
        REQUIRE(info.vdwRadius > 0.0f);
    }
    // 常见元素抽查
    CHECK(kElementTable[1].symbol == std::string("H"));
    CHECK(kElementTable[6].symbol == std::string("C"));
    CHECK(kElementTable[7].symbol == std::string("N"));
    CHECK(kElementTable[8].symbol == std::string("O"));
    CHECK(kElementTable[16].symbol == std::string("S"));
    CHECK(kElementTable[79].symbol == std::string("Au"));
    CHECK(kElementTable[118].symbol == std::string("Og"));
}

TEST_CASE("Element colors are valid sRGB", "[element]") {
    for (uint8_t i = 1; i <= 118; ++i) {
        const auto& info = kElementTable[i];
        for (float c : info.color) {
            REQUIRE(c >= 0.0f);
            REQUIRE(c <= 1.0f);
        }
    }
}

TEST_CASE("elementInfo returns Unknown for out of range", "[element]") {
    const auto& info = elementInfo(static_cast<ElementType>(0xFF));
    CHECK(info.atomicNumber == 0);
    CHECK(info.symbol == std::string("Xx"));
    const auto& zero = elementInfo(static_cast<ElementType>(0));
    CHECK(zero.atomicNumber == 0);
}

TEST_CASE("elementFromSymbol resolves symbols", "[element]") {
    CHECK(elementFromSymbol("H") == ElementType::Hydrogen);
    CHECK(elementFromSymbol("C") == ElementType::Carbon);
    CHECK(elementFromSymbol("Fe") == ElementType::Iron);
    CHECK(elementFromSymbol("Og") == ElementType::Oganesson);
    CHECK(elementFromSymbol("Xx") == ElementType::Unknown);
    CHECK(elementFromSymbol("") == ElementType::Unknown);
    CHECK(elementFromSymbol(nullptr) == ElementType::Unknown);
}
