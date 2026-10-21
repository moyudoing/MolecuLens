// =============================================================================
// MolecuLens · io · registry/parser_registry.cpp —— 解析器注册表实现
// 规格来源：主方案 §4.8
// =============================================================================

#include "io/registry/parser_registry.h"

#include <algorithm>
#include <cctype>

#include "io/cube/cube_parser.h"
#include "io/gro/gro_parser.h"
#include "io/pdb/pdb_parser.h"
#include "io/sdf/sdf_parser.h"
#include "spdlog/spdlog.h"

namespace moleculens::entity {

void ParserRegistry::registerParser(std::shared_ptr<IFileReader> parser) {
    parsers_.push_back(std::move(parser));
}

std::vector<std::string> ParserRegistry::allExtensions() const {
    std::vector<std::string> out;
    for (const auto& p : parsers_) {
        for (const auto& raw : p->extensions()) {
            std::string ext = raw;
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            // 归一化为"不含前导点"的形式（各 reader 的约定不同，这里统一）
            if (!ext.empty() && ext.front() == '.') {
                ext.erase(ext.begin());
            }
            if (ext.empty()) {
                continue;
            }
            if (std::find(out.begin(), out.end(), ext) == out.end()) {
                out.push_back(std::move(ext));
            }
        }
    }
    return out;
}

IFileReader* ParserRegistry::findForPath(const std::filesystem::path& path) {
    // 1) 扩展名表（大小写不敏感）
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto& p : parsers_) {
        for (const auto& e : p->extensions()) {
            if (ext == e || ext == "." + e) {
                return p.get();
            }
        }
    }
    // 2) 魔数嗅探 / 启发式兜底
    for (auto& p : parsers_) {
        if (p->canOpen(path)) {
            return p.get();
        }
    }
    return nullptr;
}

Result<ParseResult> ParserRegistry::parseFile(const std::filesystem::path& path,
                                              const ParseOptions& opts) {
    IFileReader* reader = findForPath(path);
    if (reader == nullptr) {
        return Result<ParseResult>::error(
            ErrorCode::FormatError,
            "No parser found for: " + path.filename().string());
    }
    auto result = reader->parse(path, opts);
    if (result) {
        result->sourceFile = path.string();
        spdlog::info("[io] parsed '{}' with '{}' reader", path.string(), reader->name());
    }
    return result;
}

ParserRegistry& ParserRegistry::defaultRegistry() {
    static ParserRegistry registry = [] {
        ParserRegistry r;
        r.registerParser(std::make_shared<PdbParser>());
        r.registerParser(std::make_shared<GroParser>());
        r.registerParser(std::make_shared<SdfParser>());
        r.registerParser(std::make_shared<CubeFileReader>());
        return r;
    }();
    return registry;
}

} // namespace moleculens::entity
