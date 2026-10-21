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

    // 全部已注册解析器支持的扩展名（小写、不含点、去重、按注册顺序）。
    // 用途：文件对话框过滤器与解析器**共用同一事实来源** —— 新增格式只注册一次，
    // UI 的"可打开文件类型"自动跟上，不会出现"解析器能读、对话框看不到"的错配。
    [[nodiscard]] std::vector<std::string> allExtensions() const;

    static ParserRegistry& defaultRegistry();   // 内建 PDB/GRO/SDF/cube

private:
    std::vector<std::shared_ptr<IFileReader>> parsers_;
};

} // namespace moleculens::entity
