// =============================================================================
// MolecuLens · io · sdf/sdf_parser.h —— SDF / MDL molfile 解析器
// 规格来源：MDL ctfile V2000（第 1 行标题；第 2 行程序；第 3 行 counts：
//           nAtoms(1-3) nBonds(4-6) … 含 V2000；atom 块 x(1-10) y(11-20)
//           z(21-30) element(31-34)；bond 块 a(1-3) b(4-6) order(7-9)；
//           M END 结束记录；$$$$ 分隔多记录）
// 实现：多记录每记录 = 一个分子（结构 + 实例）；键来自显式键表（1/2/3/4→0 芳），
//       不推断；多个记录全部挂同一体系（一次导入 = 一个体系）。
// =============================================================================

#pragma once

#include "io/registry/ifile_reader.h"

namespace moleculens::entity {

class SdfParser : public IFileReader {
public:
    const char* name() const override { return "sdf"; }
    std::vector<std::string> extensions() const override { return {"sdf", "mol"}; }
    bool canOpen(const std::filesystem::path&) const override;
    Result<ParseResult> parse(const std::filesystem::path&, const ParseOptions&) override;
};

} // namespace moleculens::entity