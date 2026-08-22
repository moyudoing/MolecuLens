// =============================================================================
// MolecuLens · io · pdb/pdb_parser.h —— PDB 解析器
// 规格来源：主方案 §4.8 表（HEADER/TITLE/COMPND/ATOM/HETATM/CONECT/TER/
//           MODEL/ENDMDL/END；HELIX/SHEET → ssHint；strict/lenient 容错）
// =============================================================================

#pragma once

#include "io/registry/ifile_reader.h"

namespace moleculens::entity {

class PdbParser : public IFileReader {
public:
    const char* name() const override { return "pdb"; }
    std::vector<std::string> extensions() const override { return {"pdb", "ent"}; }
    bool canOpen(const std::filesystem::path&) const override;
    Result<ParseResult> parse(const std::filesystem::path&, const ParseOptions&) override;
};

} // namespace moleculens::entity
