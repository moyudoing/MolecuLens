// =============================================================================
// MolecuLens · entity · selection/parse_table.h —— 选择语言符号表（ParseTable）
// 规格来源：主方案 §4.6（关键字/单字宏/字符串函数/数学函数；插件可扩展）
// =============================================================================

#pragma once

#include <functional>
#include <string_view>
#include <unordered_map>

#include "entity/scene/scene.h"

namespace moleculens::entity {

class Selection;

// 属性读写回调（Get/Set，坐标相关按帧求值）
using GetterFn = std::function<float(const SceneStructure&, const FrameData&,
                                     uint64_t atomIndex, uint64_t frame)>;
using SetterFn = std::function<void(Selection&, const SceneStructure&, const FrameData&,
                                    uint64_t frame, float value)>;
using WordFn   = std::function<void(Selection&)>;      // 布尔宏（all/none/protein/…）
using StringFn = std::function<void(Selection&, std::string_view)>;
using MathFn   = std::function<void(Selection&, float)>;

// 选择语言符号表（主方案 §4.6：关键字/单字宏/字符串函数/数学函数；插件可扩展）
// 目录粒度约束（主方案 §3.5）：本头 header-only 实现（selection 目录 ≤3 文件）
class ParseTable {
public:
    void addKeyword(std::string_view name, GetterFn getter, SetterFn setter = {}) {
        keywords_.emplace(std::string(name), std::move(getter));
        if (setter) {
            setters_.emplace(std::string(name), std::move(setter));
        }
    }
    void addSingleWord(std::string_view name, WordFn fn) {
        words_.emplace(std::string(name), std::move(fn));
    }
    void addStringFunction(std::string_view name, StringFn fn) {
        stringFns_.emplace(std::string(name), std::move(fn));
    }
    void addMathFunction(std::string_view name, MathFn fn) {
        mathFns_.emplace(std::string(name), std::move(fn));
    }

    // MSVC C++17：unordered_map 无异构查找，string_view 需转 std::string
    bool isKeyword(std::string_view name) const {
        return keywords_.count(std::string(name)) != 0;
    }
    bool isWord(std::string_view name) const {
        return words_.count(std::string(name)) != 0;
    }
    bool isStringFn(std::string_view name) const {
        return stringFns_.count(std::string(name)) != 0;
    }
    bool isMathFn(std::string_view name) const {
        return mathFns_.count(std::string(name)) != 0;
    }

    const GetterFn* getter(std::string_view name) const {
        auto it = keywords_.find(std::string(name));
        return it != keywords_.end() ? &it->second : nullptr;
    }
    const SetterFn* setter(std::string_view name) const {
        auto it = setters_.find(std::string(name));
        return it != setters_.end() ? &it->second : nullptr;
    }
    const WordFn* word(std::string_view name) const {
        auto it = words_.find(std::string(name));
        return it != words_.end() ? &it->second : nullptr;
    }
    const StringFn* stringFn(std::string_view name) const {
        auto it = stringFns_.find(std::string(name));
        return it != stringFns_.end() ? &it->second : nullptr;
    }
    const MathFn* mathFn(std::string_view name) const {
        auto it = mathFns_.find(std::string(name));
        return it != mathFns_.end() ? &it->second : nullptr;
    }

    // 默认符号表（实现见 selection.cpp：单字宏 all/none/protein/backbone/…）
    static const ParseTable& defaultTable();

private:
    std::unordered_map<std::string, GetterFn> keywords_;
    std::unordered_map<std::string, SetterFn> setters_;
    std::unordered_map<std::string, WordFn> words_;
    std::unordered_map<std::string, StringFn> stringFns_;
    std::unordered_map<std::string, MathFn> mathFns_;
};

} // namespace moleculens::entity
