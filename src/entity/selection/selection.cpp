// =============================================================================
// MolecuLens · entity · selection/selection.cpp —— 选择语言解析与求值
// 规格来源：主方案 §4.6（递归下降；AND/OR/NOT；属性比较；within/same residue）
// 求值按帧：坐标相关关键字（within）取指定帧。
// =============================================================================

#include "entity/selection/selection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace moleculens::entity {

// —— 语法树节点（定义于 selection.h detail 命名空间）——
namespace {

// 比较运算符别名（detail::SelectionNode::Comparison）
using CmpOp = detail::SelectionNode::Comparison::Op;
using Comparison = detail::SelectionNode::Comparison;

} // namespace

// 默认符号表（主方案 §4.6 单字宏）
const ParseTable& ParseTable::defaultTable() {
    static const ParseTable table = [] {
        ParseTable t;
        t.addSingleWord("all", [](Selection& s) { s.all(true); });
        t.addSingleWord("none", [](Selection& s) { s.all(false); });
        // protein/backbone/sidechain/waters：残基表完整判定由解析器扩展 ——
        // 目前是**占位**（回调不产生任何掩码）。以 implemented=false 注册，使 UI
        // 能把它置灰：选中一个占位宏会让掩码保持为空 → 表示直接消失且无报错。
        t.addSingleWord("protein", [](Selection&) {}, /*implemented=*/false);
        t.addSingleWord("backbone", [](Selection&) {}, /*implemented=*/false);
        t.addSingleWord("sidechain", [](Selection&) {}, /*implemented=*/false);
        t.addSingleWord("waters", [](Selection&) {}, /*implemented=*/false);
        return t;
    }();
    return table;
}

// "关键字 比较符 值" 形式支持的关键字。
// **单一事实来源**：isBuiltinAttr（解析器分派）与 selectionKeywords（UI 列表）
// 都查这张表 —— 两处各写一份时，UI 会列出解析器不认识的关键字。
namespace {
const char* const kBuiltinAttrs[] = {"name",   "resname", "resid",  "chain",
                                     "element", "occupancy", "bfactor"};
constexpr size_t kBuiltinAttrCount = sizeof(kBuiltinAttrs) / sizeof(kBuiltinAttrs[0]);
} // namespace

const char* const* selectionKeywords(size_t* count) {
    if (count != nullptr) {
        *count = kBuiltinAttrCount;
    }
    return kBuiltinAttrs;
}

namespace {

// —— 分词器 ——
struct Token {
    enum class Kind : uint8_t { Word, Number, LParen, RParen, Op, End };
    Kind kind = Kind::End;
    std::string text;
    float number = 0.0f;
};

class Lexer {
public:
    explicit Lexer(const std::string& s) : s_(s) {}

    Token next() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
        if (pos_ >= s_.size()) {
            return {};
        }
        char c = s_[pos_];
        if (c == '(') { ++pos_; return {Token::Kind::LParen, "("}; }
        if (c == ')') { ++pos_; return {Token::Kind::RParen, ")"}; }
        if (c == '=' || c == '!' || c == '<' || c == '>') {
            std::string op(1, c);
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] == '=') {
                op += '=';
                ++pos_;
            }
            return {Token::Kind::Op, op};
        }
        // 数字（含负数）
        bool neg = false;
        if (c == '-') {
            neg = true;
            ++pos_;
            if (pos_ >= s_.size() ||
                !std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                return {Token::Kind::Op, "-"};
            }
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || neg) {
            std::string num;
            if (neg) {
                num += '-';
            }
            while (pos_ < s_.size() &&
                   (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.')) {
                num += s_[pos_++];
            }
            return {Token::Kind::Number, num, std::strtof(num.c_str(), nullptr)};
        }
        // 单词
        std::string w;
        while (pos_ < s_.size()) {
            char cc = s_[pos_];
            if (std::isalnum(static_cast<unsigned char>(cc)) || cc == '_' ||
                cc == '*' || cc == '?') {
                w += cc;
                ++pos_;
            } else {
                break;
            }
        }
        return {Token::Kind::Word, w};
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
};

bool eqStr(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool cmpFloat(float a, CmpOp op, float b) {
    switch (op) {
        case CmpOp::Eq: return std::fabs(a - b) < 1e-4f;
        case CmpOp::Ne: return std::fabs(a - b) >= 1e-4f;
        case CmpOp::Lt: return a < b;
        case CmpOp::Gt: return a > b;
        case CmpOp::Le: return a <= b;
        case CmpOp::Ge: return a >= b;
        case CmpOp::Range: return a >= b && a <= b;
        default: return false;
    }
}

bool isBuiltinAttr(const std::string& a) {
    for (const char* k : kBuiltinAttrs) {
        if (a == k) {
            return true;
        }
    }
    return false;
}

// —— 递归下降解析 ——
class Parser {
public:
    Parser(const std::string& expr, const ParseTable& table)
        : lexer_(expr), table_(table) {
        token_ = lexer_.next();
    }

    std::unique_ptr<detail::SelectionNode> parseExpr() {
        auto lhs = parseFactor();
        if (!lhs) {
            return nullptr;
        }
        while (token_.kind == Token::Kind::Word &&
               (eqStr(token_.text, "AND") || eqStr(token_.text, "OR"))) {
            const bool isAnd = eqStr(token_.text, "AND");
            advance();
            auto rhs = parseFactor();
            if (!rhs) {
                return nullptr;
            }
            auto node = std::make_unique<detail::SelectionNode>();
            node->kind = isAnd ? detail::SelectionNode::Kind::And
                               : detail::SelectionNode::Kind::Or;
            node->lhs = std::move(lhs);
            node->rhs = std::move(rhs);
            lhs = std::move(node);
        }
        return lhs;
    }

private:
    using Node = detail::SelectionNode;

    std::unique_ptr<Node> parseFactor() {
        if (token_.kind == Token::Kind::Word && eqStr(token_.text, "NOT")) {
            advance();
            auto inner = parseFactor();
            if (!inner) {
                return nullptr;
            }
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Not;
            node->lhs = std::move(inner);
            return node;
        }
        if (token_.kind == Token::Kind::LParen) {
            advance();
            auto inner = parseExpr();
            if (!inner || token_.kind != Token::Kind::RParen) {
                return nullptr;
            }
            advance();
            return inner;
        }
        return parsePredicate();
    }

    std::unique_ptr<Node> parsePredicate() {
        if (token_.kind != Token::Kind::Word) {
            return nullptr;
        }
        const std::string first = token_.text;

        // within <dist> of <predicate>
        if (eqStr(first, "within")) {
            advance();
            if (token_.kind != Token::Kind::Number) {
                return nullptr;
            }
            const float dist = token_.number;
            advance();
            if (token_.kind != Token::Kind::Word || !eqStr(token_.text, "of")) {
                return nullptr;
            }
            advance();
            auto inner = parsePredicate();
            if (!inner) {
                return nullptr;
            }
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Within;
            node->dist = dist;
            node->lhs = std::move(inner);
            return node;
        }

        // same residue as <predicate>
        if (eqStr(first, "same")) {
            if (!eqStr(peekWord(), "residue")) {
                return nullptr;
            }
            advance();
            if (token_.kind != Token::Kind::Word || !eqStr(token_.text, "residue")) {
                return nullptr;
            }
            advance();
            if (token_.kind != Token::Kind::Word || !eqStr(token_.text, "as")) {
                return nullptr;
            }
            advance();
            auto inner = parsePredicate();
            if (!inner) {
                return nullptr;
            }
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::SameResidue;
            node->lhs = std::move(inner);
            return node;
        }

        // 单字宏（all/none/protein/backbone/sidechain/waters/自定义）
        if (table_.isWord(first) || eqStr(first, "all") || eqStr(first, "none")) {
            advance();
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Word;
            node->word = first;
            return node;
        }

        // 属性比较：attr [op] value / attr value-range
        if (!table_.isKeyword(first) && !isBuiltinAttr(first)) {
            return nullptr;
        }
        const std::string attr = first;
        advance();
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Cmp;
        node->cmp.attr = attr;

        if (token_.kind == Token::Kind::Op) {
            const std::string op = token_.text;
            advance();
            if (op == "=") node->cmp.op = CmpOp::Eq;
            else if (op == "!=") node->cmp.op = CmpOp::Ne;
            else if (op == "<") node->cmp.op = CmpOp::Lt;
            else if (op == ">") node->cmp.op = CmpOp::Gt;
            else if (op == "<=") node->cmp.op = CmpOp::Le;
            else if (op == ">=") node->cmp.op = CmpOp::Ge;
            else return nullptr;
        }

        if (token_.kind == Token::Kind::Number) {
            node->cmp.valF = token_.number;
            advance();
            if (token_.kind == Token::Kind::Op && token_.text == "-") {
                advance();
                if (token_.kind != Token::Kind::Number) {
                    return nullptr;
                }
                node->cmp.rangeLo = node->cmp.valF;
                node->cmp.rangeHi = token_.number;
                node->cmp.op = CmpOp::Range;
                advance();
            }
        } else if (token_.kind == Token::Kind::Word) {
            node->cmp.valS = token_.text;
            advance();
        } else {
            return nullptr;
        }
        return node;
    }

    std::string peekWord() {
        Lexer saved = lexer_;
        Token t = saved.next();
        return t.kind == Token::Kind::Word ? t.text : std::string();
    }

    void advance() { token_ = lexer_.next(); }

    Lexer lexer_;
    const ParseTable& table_;
    Token token_;
};

// —— 求值 ——
class Evaluator {
public:
    Evaluator(Selection& sel, const SceneStructure& scene, const FrameData& frame,
              uint64_t frameIndex, const ParseTable* table)
        : sel_(sel), scene_(scene), frame_(frame), frameIndex_(frameIndex), table_(table) {}

    void eval(const detail::SelectionNode* node) {
        if (!node) {
            return;
        }
        switch (node->kind) {
            case detail::SelectionNode::Kind::And: {
                // 先清空再求两侧（避免残留 mask 污染，主方案 §4.6 语义）
                sel_.clearMask();
                eval(node->lhs.get());
                std::vector<bool> a = sel_.mask();
                sel_.clearMask();
                eval(node->rhs.get());
                std::vector<bool> b = sel_.mask();
                sel_.clearMask();
                for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
                    if (a[i] && b[i]) {
                        sel_.addToMask(i);
                    }
                }
                break;
            }
            case detail::SelectionNode::Kind::Or: {
                sel_.clearMask();
                eval(node->lhs.get());
                std::vector<bool> a = sel_.mask();
                sel_.clearMask();
                eval(node->rhs.get());
                std::vector<bool> b = sel_.mask();
                sel_.clearMask();
                for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
                    if (a[i] || b[i]) {
                        sel_.addToMask(i);
                    }
                }
                break;
            }
            case detail::SelectionNode::Kind::Not: {
                eval(node->lhs.get());
                std::vector<bool> m = sel_.mask();
                sel_.clearMask();
                for (size_t i = 0; i < m.size(); ++i) {
                    if (!m[i]) {
                        sel_.addToMask(i);
                    }
                }
                break;
            }
            case detail::SelectionNode::Kind::Word: {
                if (const WordFn* fn = table_->word(node->word)) {
                    (*fn)(sel_);
                } else if (eqStr(node->word, "all")) {
                    sel_.all(true);
                } else if (eqStr(node->word, "none")) {
                    sel_.all(false);
                } else {
                    sel_.all(false);
                }
                break;
            }
            case detail::SelectionNode::Kind::Cmp:
                evalCmp(node->cmp);
                break;
            case detail::SelectionNode::Kind::Within: {
                std::vector<bool> ref = evaluateSub(node->lhs.get());
                const auto* pos = frame_.positions;
                sel_.clearMask();
                if (!pos) {
                    break;
                }
                const float d2 = node->dist * node->dist;
                for (size_t i = 0; i < ref.size() && i < pos->size(); ++i) {
                    if (!ref[i]) {
                        continue;
                    }
                    for (size_t j = 0; j < pos->size(); ++j) {
                        const glm::vec3 d = (*pos)[j] - (*pos)[i];
                        if (glm::dot(d, d) <= d2) {
                            sel_.addToMask(j);
                        }
                    }
                }
                break;
            }
            case detail::SelectionNode::Kind::SameResidue: {
                std::vector<bool> ref = evaluateSub(node->lhs.get());
                sel_.clearMask();
                uint32_t g = 0;
                for (const auto& inst : scene_.instances()) {
                    if (inst.structureIndex >= scene_.structures().size()) {
                        continue;
                    }
                    const MoleculeStructure& st =
                        scene_.structures()[inst.structureIndex];
                    const size_t n = st.elements.size();
                    for (size_t i = 0; i < n; ++i, ++g) {
                        if (!ref[g]) {
                            continue;
                        }
                        const int32_t rid = st.atoms[i].residueId;
                        const int32_t cid = st.atoms[i].chainId;
                        const uint32_t gbase = g - static_cast<uint32_t>(i);
                        for (size_t j = 0; j < n; ++j) {
                            if (st.atoms[j].residueId == rid &&
                                st.atoms[j].chainId == cid &&
                                gbase + j < ref.size()) {
                                sel_.addToMask(gbase + static_cast<uint32_t>(j));
                            }
                        }
                    }
                }
                break;
            }
        }
    }

private:
    // 求子表达式并返回其掩码（不污染当前 mask）
    std::vector<bool> evaluateSub(const detail::SelectionNode* n) {
        const std::vector<bool> saved = sel_.mask();
        sel_.clearMask();
        eval(n);
        std::vector<bool> result = sel_.mask();
        sel_.clearMask();
        for (size_t i = 0; i < saved.size(); ++i) {
            if (saved[i]) {
                sel_.addToMask(i);
            }
        }
        return result;
    }

    void evalCmp(const Comparison& c) {
        sel_.clearMask();
        uint32_t g = 0;
        for (const auto& inst : scene_.instances()) {
            if (inst.structureIndex >= scene_.structures().size()) {
                continue;
            }
            const MoleculeStructure& st = scene_.structures()[inst.structureIndex];
            const size_t n = st.elements.size();
            for (size_t i = 0; i < n; ++i, ++g) {
                bool hit = false;
                if (c.attr == "name") {
                    hit = eqStr(std::string(scene_.atomName(inst.structureIndex,
                                                            static_cast<uint32_t>(i))),
                                c.valS);
                } else if (c.attr == "resname") {
                    // 残基名（VMD 的 resname）：水 = SOL/WAT/HOH/TIP3 等，可写
                    // "not resname SOL" 只留溶质（对齐 VMD 的 "not water" 用法）。
                    const int32_t rid = st.atoms[i].residueId;
                    hit = rid >= 0 && eqStr(std::string(scene_.residueName(rid)), c.valS);
                } else if (c.attr == "resid") {
                    const float r = static_cast<float>(st.atoms[i].residueId);
                    hit = c.op == CmpOp::Range ? (r >= c.rangeLo && r <= c.rangeHi)
                                               : cmpFloat(r, c.op, c.valF);
                } else if (c.attr == "chain") {
                    hit = eqStr(std::string(scene_.chainName(st.atoms[i].chainId)), c.valS);
                } else if (c.attr == "element") {
                    const char* sym = elementInfo(st.elements[i]).symbol;
                    hit = eqStr(sym, c.valS);
                } else if (c.attr == "occupancy") {
                    hit = cmpFloat(static_cast<float>(st.atoms[i].occupancy), c.op, c.valF);
                } else if (c.attr == "bfactor") {
                    hit = cmpFloat(static_cast<float>(st.atoms[i].bfactor) / 100.0f,
                                   c.op, c.valF);
                } else if (const GetterFn* getter = table_->getter(c.attr)) {
                    hit = cmpFloat((*getter)(scene_, frame_, g, frameIndex_), c.op, c.valF);
                }
                if (hit) {
                    sel_.addToMask(g);
                }
            }
        }
    }

    Selection& sel_;
    const SceneStructure& scene_;
    const FrameData& frame_;
    uint64_t frameIndex_;
    const ParseTable* table_;
};

} // namespace

bool Selection::parse(const std::string& expr, const ParseTable& symbols) {
    expr_ = expr;
    table_ = &symbols;
    valid_ = false;
    root_.reset();

    Parser parser(expr, symbols);
    root_ = parser.parseExpr();
    valid_ = root_ != nullptr;
    return valid_;
}

void Selection::evaluate(const SceneStructure& scene, const FrameData& frame,
                         uint64_t frameIndex) {
    mask_.assign(scene.coordsSoA().atomIds.size(), false);
    if (!valid_ || !root_) {
        return;
    }
    Evaluator ev(*this, scene, frame, frameIndex, table_);
    ev.eval(root_.get());
}

uint64_t Selection::selected() const {
    uint64_t count = 0;
    for (bool b : mask_) {
        count += b ? 1 : 0;
    }
    return count;
}

} // namespace moleculens::entity
