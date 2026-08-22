// =============================================================================
// MolecuLens · io · gro/gro_parser.h —— GRO 解析器
// 规格来源：主方案 §4.8（固定列宽；resnr/resname/atomname/atomnr/x,y,z(nm)/
//           [vx,vy,vz]；nm→Å 换算 ×10；box 行 3/9 浮点；无键 → 启发式）
// =============================================================================

#pragma once

#include "io/registry/ifile_reader.h"

namespace moleculens::entity {

class GroParser : public IFileReader {
public:
    const char* name() const override { return "gro"; }
    std::vector<std::string> extensions() const override { return {"gro"}; }
    bool canOpen(const std::filesystem::path&) const override;
    Result<ParseResult> parse(const std::filesystem::path&, const ParseOptions&) override;
};

} // namespace moleculens::entity
