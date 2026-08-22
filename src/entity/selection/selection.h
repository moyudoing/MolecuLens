// =============================================================================
// MolecuLens · entity · selection/selection.h —— 选择语言（Selection）
// 规格来源：主方案 §4.6
//   选择表达式 := 因子 (AND|OR|NOT 因子)*
//   因子       := 关键字 | 属性比较 | "(" 表达式 ")"
// 求值按帧：坐标相关关键字（within/x,y,z）取指定帧。
// =============================================================================

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "entity/scene/scene.h"
#include "entity/selection/parse_table.h"

namespace moleculens::entity {

namespace detail {

// 语法树节点（Selection 私有实现；完整定义于此保证跨编译单元 unique_ptr 析构）
struct SelectionNode {
    enum class Kind : uint8_t { And, Or, Not, Word, Cmp, Within, SameResidue } kind;
    std::string word;                        // Word
    struct Comparison {                      // Cmp（属性比较）
        std::string attr;
        enum class Op : uint8_t { Eq, Ne, Lt, Gt, Le, Ge, Range } op = Op::Eq;
        float valF = 0.0f;
        std::string valS;
        float rangeLo = 0.0f, rangeHi = 0.0f;
    } cmp;
    std::unique_ptr<SelectionNode> lhs, rhs; // And/Or/Not/Within/SameResidue
    float dist = 0.0f;                       // Within
};

} // namespace detail

class Selection {
public:
    ~Selection() = default;   // SelectionNode 完整类型已在本头定义（unique_ptr 析构安全）

    // 语法树解析；symbols 可缺省 → defaultTable()
    bool parse(const std::string& expr, const ParseTable& symbols);

    // 求值：按当前帧数据输出原子掩码
    void evaluate(const SceneStructure& scene, const FrameData& frame, uint64_t frameIndex);

    // 对全场景求值（内部调用 evaluate）
    const std::vector<bool>& mask() const { return mask_; }
    uint64_t selected() const;               // 掩码汇总

    const std::string& expression() const { return expr_; }
    bool valid() const { return valid_; }

    // 语法树求值接口（ParseTable 回调用）
    void addToMask(uint64_t index) { if (index < mask_.size()) mask_[index] = true; }
    void clearMask() { std::fill(mask_.begin(), mask_.end(), false); }
    void all(bool on) { std::fill(mask_.begin(), mask_.end(), on); }

private:
    std::unique_ptr<detail::SelectionNode> root_;
    std::string expr_;
    std::vector<bool> mask_;
    bool valid_ = false;
    const ParseTable* table_ = nullptr;
};

} // namespace moleculens::entity
