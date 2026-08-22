// =============================================================================
// MolecuLens · io · registry/ifile_reader.h —— 文件解析插件接口
// 规格来源：主方案 §4.8（两级识别：扩展名表 → 魔数嗅探 → 文本启发式兜底）
// =============================================================================

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "entity/result.h"
#include "entity/scene/scene.h"

namespace moleculens::entity {

struct ParseWarning {
    size_t line = 0;
    std::string message;
};

struct ParseOptions {
    bool inferBonds = true;
    bool strict = false;
    Units targetUnits = Units::Angstrom;
};

struct ParseResult {
    std::shared_ptr<SceneStructure> scene;
    std::vector<ParseWarning> warnings;   // { line, message }
    std::string sourceFile;
    uint64_t frameCount = 1;              // 轨迹帧数（PDB MODEL 多帧）
    float dt = 2.0f;
};

// "file reader" 插件接口（主方案 §10.2）
class IFileReader {
public:
    virtual ~IFileReader() = default;

    virtual const char* name() const = 0;                    // "pdb" | "gro"
    virtual std::vector<std::string> extensions() const = 0; // { "pdb", "ent" }
    virtual bool canOpen(const std::filesystem::path&) const = 0;  // 魔数嗅探
    virtual Result<ParseResult> parse(const std::filesystem::path&,
                                      const ParseOptions&) = 0;
};

} // namespace moleculens::entity
