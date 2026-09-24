// =============================================================================
// MolecuLens · io · sdf/sdf_parser.cpp —— SDF / MDL molfile 解析器实现
// 规格来源：MDL ctfile V2000（见 sdf_parser.h 头注释）
//   - 记录：标题/程序/counts 行 → atom 块（x y z element …）→ bond 块
//     （a b order …）→ "M  END"；记录间 "$$$$"
//   - 键表显式：order 1/2/3 直用，4(芳)→0；不推断（opts.inferBonds 不适用）
//   - 多记录（$$$$）：每记录一个分子结构 + 实例，全挂同一体系
//   - V3000 等不支持的 counts（无 "V2000"）→ 记警告并跳过该记录
// 单位：坐标直接为 Å（V2000 规范），无需换算
// =============================================================================

#include "io/sdf/sdf_parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t");
    const size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

// 固定区间字段（1 基列）：col1..col2（含）→ 截子串去首尾空白；越界返回空
std::string field(const std::string& line, int col1, int col2) {
    const size_t begin = static_cast<size_t>(std::max(col1, 1) - 1);
    const size_t end = static_cast<size_t>(col2);
    if (begin >= line.size()) {
        return {};
    }
    return trim(line.substr(begin, end > line.size() ? line.size() - begin : end - begin));
}

int parseFieldInt(const std::string& s, int fallback) {
    if (s.empty()) {
        return fallback;
    }
    try {
        size_t pos = 0;
        const int v = std::stoi(s, &pos);
        return pos == 0 ? fallback : v;
    } catch (...) {
        return fallback;
    }
}

float parseFieldFloat(const std::string& s, float fallback) {
    if (s.empty()) {
        return fallback;
    }
    try {
        size_t pos = 0;
        const float v = std::stof(s, &pos);
        return pos == 0 ? fallback : v;
    } catch (...) {
        return fallback;
    }
}

// 键序 → 项目 Bond.order（0 芳 / 1 / 2 / 3）
uint8_t normalizeBondOrder(int order) {
    switch (order) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        default: return 0;   // 4(芳) 及异常值 → 0(芳)
    }
}

} // namespace

bool SdfParser::canOpen(const std::filesystem::path& path) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    // counts 行可能不在第 3 行（部分写入器在“程序名”与 counts 间多个空行）：
    // 扫描前几行，任一含 "V2000"/"V3000" 即认为是 MDL molfile。
    std::string line;
    const char* names[] = {"V2000", "V3000"};
    for (int i = 0; i < 6 && std::getline(in, line); ++i) {
        for (const char* n : names) {
            if (line.find(n) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

Result<ParseResult> SdfParser::parse(const std::filesystem::path& path,
                                     const ParseOptions& opts) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<ParseResult>::error(ErrorCode::IoError,
                                          "Cannot open file: " + path.string());
    }

    auto scene = std::make_shared<SceneStructure>();
    ParseResult result;
    result.sourceFile = path.string();
    result.scene = scene;

    auto warn = [&](size_t line, const std::string& msg) {
        if (opts.strict) {
            return false;
        }
        result.warnings.push_back({line, msg});
        spdlog::warn("[io] SDF line {}: {}", line, msg);
        return true;
    };

    // 链/残基名：SDF 无残基，用固定 "MOL"（残基配色可用）
    const int32_t cid = scene->addChain("1");
    const int32_t rid = scene->addResidue("MOL");

    std::string line;
    size_t lineNo = 0;
    int moleculeCount = 0;

    // 每个记录一个分子；一次解析 = 一个体系
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // 标题行（也可能为空）。跳过空行
        const std::string title = trim(line);
        if (title.empty()) {
            continue;
        }
        // 第 2 行：程序名（忽略）——但可能是 $$ 分隔符出现在标题位置
        if (title == "$$$$") {
            continue;
        }

        std::string progLine;
        if (!std::getline(in, progLine)) {
            break;
        }
        ++lineNo;
        // counts 行可能被空行隔开（部分写入器）：跳过空白直到首个非空行
        std::string counts;
        for (int blankGuard = 0; blankGuard < 4; ++blankGuard) {
            std::string cand;
            if (!std::getline(in, cand)) {
                counts.clear();
                break;
            }
            ++lineNo;
            if (!trim(cand).empty()) {
                counts = cand;
                break;
            }
        }
        if (trim(counts).empty()) {
            break;   // 未找到 counts 行（记录异常/文件结束）
        }

        if (counts.find("V2000") == std::string::npos) {
            if (!warn(lineNo, "unsupported SDF/MOL version (only V2000 supported), "
                              "record skipped: '" + title + "'")) {
                return Result<ParseResult>::error(ErrorCode::ParseError,
                                                  "SDF unsupported counts line");
            }
            // 跳过本记录剩余内容到 $$$$
            bool skipDone = false;
            for (; std::getline(in, line); ++lineNo) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (trim(line) == "$$$$") {
                    skipDone = true;
                    break;
                }
            }
            if (!skipDone) {
                break;
            }
            continue;
        }

        const int nAtoms = parseFieldInt(field(counts, 1, 3), 0);
        const int nBonds = parseFieldInt(field(counts, 4, 6), 0);
        if (nAtoms <= 0 || nAtoms > 1000000) {
            if (!warn(lineNo, "invalid atom count in counts line")) {
                return Result<ParseResult>::error(ErrorCode::ParseError,
                                                  "SDF invalid counts line");
            }
            continue;
        }

        // atom 块
        std::vector<ElementType> elements;
        std::vector<Atom> atoms;
        std::vector<std::string> atomNames;
        std::vector<glm::vec3> positions;
        elements.reserve(static_cast<size_t>(nAtoms));
        for (int i = 0; i < nAtoms; ++i) {
            if (!std::getline(in, line)) {
                break;
            }
            ++lineNo;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const float x = parseFieldFloat(field(line, 1, 10), 0.0f);
            const float y = parseFieldFloat(field(line, 11, 20), 0.0f);
            const float z = parseFieldFloat(field(line, 21, 30), 0.0f);
            const std::string elStr = field(line, 31, 34);
            // V2000 无原子名列：原子名兜底用元素符号（选择语言/分子列表可辨识）
            const std::string atomName = elStr.empty() ? std::string("?") : elStr;
            const ElementType el = elementFromSymbol(elStr.c_str());

            elements.push_back(el);
            Atom a;
            a.element = el;
            a.occupancy = 100;
            a.bfactor = 0;
            a.residueId = rid;
            a.chainId = cid;
            atoms.push_back(a);
            atomNames.push_back(atomName);
            positions.push_back(glm::vec3(x, y, z));
        }

        // bond 块
        std::vector<Bond> bonds;
        bonds.reserve(static_cast<size_t>(nBonds));
        for (int i = 0; i < nBonds; ++i) {
            if (!std::getline(in, line)) {
                break;
            }
            ++lineNo;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const int a = parseFieldInt(field(line, 1, 3), 0) - 1;
            const int b = parseFieldInt(field(line, 4, 6), 0) - 1;
            const int order = parseFieldInt(field(line, 7, 9), 1);
            if (a < 0 || b < 0 || a >= nAtoms || b >= nAtoms || a == b) {
                continue;
            }
            bonds.push_back({(uint32_t)std::min(a, b), (uint32_t)std::max(a, b),
                             normalizeBondOrder(order)});
        }

        // 跳过剩余记录到 M END / $$$$
        bool endReached = false;
        for (; std::getline(in, line); ++lineNo) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::string t = trim(line);
            if (t == "M  END" || t == "$$$$") {
                endReached = true;
                if (t == "$$$$") {
                    // $$$$ 已消费，是下一记录的分隔符；返回外层循环时行已被吃——用标记
                    // 重新处理：这里把该行作为记录分隔处理
                    break;
                }
                break;
            }
        }
        // M END 后若紧跟 $$$$，外层循环会在下一行读到它（标题位置），已被 continue 处理

        if (elements.empty()) {
            if (moleculeCount == 0) {
                return Result<ParseResult>::error(ErrorCode::ParseError,
                                                  "SDF contains no atoms: " + path.string());
            }
            continue;
        }

        // 一个记录 = 一个分子（结构 + 实例，挂当前体系）
        MoleculeStructure ms;
        ms.elements = std::move(elements);
        ms.atoms = std::move(atoms);
        ms.atomNameIds.resize(ms.atoms.size());
        for (size_t i = 0; i < ms.atoms.size(); ++i) {
            ms.atomNameIds[i] =
                scene->strings().intern(atomNames[i].empty() ? std::string("?") : atomNames[i]);
        }
        ms.bonds = std::move(bonds);
        ms.key = computeStructureKey(ms);
        const uint32_t structureIndex = scene->registerStructure(ms);

        auto traj = std::make_shared<Trajectory>();
        Timestep ts;
        ts.positions = std::move(positions);
        traj->frames.push_back(std::move(ts));
        traj->dt = result.dt;

        const std::string instName =
            title.empty() ? ("mol " + std::to_string(moleculeCount + 1))
                          : (title + " " + std::to_string(moleculeCount + 1));
        scene->addTrajectoryInstance(structureIndex, Transform{}, traj, UINT32_MAX,
                                     instName, false);
        ++moleculeCount;
        (void)endReached;
    }

    if (moleculeCount == 0) {
        return Result<ParseResult>::error(ErrorCode::ParseError,
                                          "SDF contains no molecules: " + path.string());
    }

    scene->setDirty(DIRTY_ALL);
    scene->bumpSerial();

    spdlog::info("[io] SDF loaded: {} molecules from {}", moleculeCount,
                 path.filename().string());
    return result;
}

} // namespace moleculens::entity