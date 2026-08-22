// =============================================================================
// MolecuLens · io · registry/parser_registry.h —— 解析器注册表
// 规格来源：主方案 §4.8（扩展名表 → 魔数嗅探 → 文本启发式兜底）
// =============================================================================

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "entity/result.h"
#include "io/registry/ifile_reader.h"

namespace moleculens::entity {

class ParserRegistry {
public:
    void registerParser(std::shared_ptr<IFileReader>);
    // 两级识别：扩展名 → 魔数/启发式
    IFileReader* findForPath(const std::filesystem::path&);
    // 便捷：parse 一站式
    Result<ParseResult> parseFile(const std::filesystem::path&, const ParseOptions& = {});

    static ParserRegistry& defaultRegistry();   // 内建 PDB/GRO

private:
    std::vector<std::shared_ptr<IFileReader>> parsers_;
};

} // namespace moleculens::entity
