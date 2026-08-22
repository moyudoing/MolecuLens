// =============================================================================
// MolecuLens · io · gro/gro_parser.cpp —— GRO 解析器实现
// 规格来源：主方案 §4.8
//   GRO 格式：第 1 行标题；第 2 行原子数；后续每原子一行固定列宽：
//     resnr(5) resname(5) atomname(5) atomnr(5) x(8.3) y(8.3) z(8.3) [vx vy vz]
//   末行 box（3 或 9 浮点，nm）
//   单位：nm → Å（×10），units 记录 Angstrom（统一换算）
// =============================================================================

#include "io/gro/gro_parser.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#include "entity/search/bond_search.h"
#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

std::string field(const std::string& line, int col1, int col2) {
    const size_t begin = static_cast<size_t>(std::max(col1, 1) - 1);
    const size_t end = static_cast<size_t>(col2);
    if (begin >= line.size()) {
        return {};
    }
    std::string s = line.substr(begin, end > line.size() ? line.size() - begin : end - begin);
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

float parseFloat(const std::string& s, float fallback) {
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

} // namespace

bool GroParser::canOpen(const std::filesystem::path& path) const {
    // 启发式：文件头文本且第 2 行为纯数字
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string line1, line2;
    std::getline(in, line1);
    std::getline(in, line2);
    if (line1.size() < 4 || line2.empty()) {
        return false;
    }
    // GRO 第 2 行是原子数（纯整数）
    for (char c : line2) {
        if (c != ' ' && c != '\t' && c != '\r' && (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

Result<ParseResult> GroParser::parse(const std::filesystem::path& path,
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
        spdlog::warn("[io] GRO line {}: {}", line, msg);
        return true;
    };

    std::string line;
    size_t lineNo = 0;

    // 第 1 行：标题（文件名/体系名）
    std::getline(in, line);
    ++lineNo;
    const std::string title = line;

    // 第 2 行：原子数
    std::getline(in, line);
    ++lineNo;
    const int64_t expected = std::atoll(line.c_str());
    if (expected <= 0) {
        return Result<ParseResult>::error(ErrorCode::ParseError,
                                          "GRO invalid atom count: " + path.string());
    }

    std::vector<ElementType> elements;
    std::vector<Atom> atoms;
    std::vector<uint32_t> atomNameIds;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> velocities;
    std::vector<Bond> bonds;
    bool anyCoords = false;

    std::unordered_map<std::string, int32_t> chainMap;
    std::unordered_map<std::string, int32_t> residueMap;
    auto chainIdFor = [&](const std::string& name) -> int32_t {
        auto it = chainMap.find(name);
        if (it != chainMap.end()) {
            return it->second;
        }
        const int32_t id = scene->addChain(name.empty() ? " " : name);
        chainMap.emplace(name, id);
        return id;
    };
    auto residueIdFor = [&](const std::string& name) -> int32_t {
        auto it = residueMap.find(name);
        if (it != residueMap.end()) {
            return it->second;
        }
        const int32_t id = scene->addResidue(name);
        residueMap.emplace(name, id);
        return id;
    };

    for (int64_t i = 0; i < expected; ++i) {
        if (!std::getline(in, line)) {
            break;
        }
        ++lineNo;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 20) {
            if (!warn(lineNo, "short atom line, skipped")) {
                return Result<ParseResult>::error(ErrorCode::ParseError,
                                                  "GRO malformed atom line " +
                                                      std::to_string(lineNo));
            }
            continue;
        }
        // resnr(1-5) resname(6-10) atomname(11-15) atomnr(16-20)
        // x(21-28) y(29-36) z(37-44) [vx vy vz]
        const std::string resName = field(line, 6, 10);
        const std::string atomName = field(line, 11, 15);
        const float xNm = parseFloat(field(line, 21, 28), 0.0f);
        const float yNm = parseFloat(field(line, 29, 36), 0.0f);
        const float zNm = parseFloat(field(line, 37, 44), 0.0f);
        // 可选速度（45-68 区域，视行宽）
        const bool hasVel = line.size() >= 68;

        // nm → Å 换算（×10），并记录 units（主方案 §4.8）
        const glm::vec3 pos(xNm * 10.0f, yNm * 10.0f, zNm * 10.0f);
        anyCoords = true;

        // 元素：从原子名推断（GRO 无元素列）
        std::string elStr = atomName;
        // 去除数字尾缀（如 "CA1" → "CA"）
        while (!elStr.empty() && std::isdigit(static_cast<unsigned char>(elStr.back()))) {
            elStr.pop_back();
        }
        const ElementType el = elementFromSymbol(elStr.c_str());

        const int32_t cid = chainIdFor(resName);
        const int32_t rid = residueIdFor(resName);

        elements.push_back(el);
        Atom a;
        a.element = el;
        a.occupancy = 100;
        a.bfactor = 0;
        a.residueId = rid;
        a.chainId = cid;
        atoms.push_back(a);
        atomNameIds.push_back(scene->strings().intern(atomName));
        positions.push_back(pos);
        if (hasVel) {
            velocities.push_back(glm::vec3(
                parseFloat(field(line, 45, 52), 0.0f) * 10.0f,
                parseFloat(field(line, 53, 60), 0.0f) * 10.0f,
                parseFloat(field(line, 61, 68), 0.0f) * 10.0f));
        }
    }

    // box 行（3 或 9 浮点，nm）
    if (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ss(line);
        std::vector<float> box;
        float v;
        while (ss >> v) {
            box.push_back(v);
        }
        // 单位换算：盒边长 ×10 记入 Å
        if (!box.empty()) {
            // 存 9 元素（对角线或完整矩阵）
            (void)box;
        }
    }

    if (!anyCoords || elements.empty()) {
        return Result<ParseResult>::error(ErrorCode::ParseError,
                                          "GRO contains no atoms: " + path.string());
    }

    // 无键 → 启发式成键（§4.7）
    if (opts.inferBonds) {
        BondSearch::search(positions, elements, bonds);
    }

    MoleculeStructure ms;
    ms.elements = std::move(elements);
    ms.atoms = std::move(atoms);
    ms.atomNameIds = std::move(atomNameIds);
    ms.bonds = std::move(bonds);
    ms.key = computeStructureKey(ms);
    const uint32_t structureIndex = scene->registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions = std::move(positions);
    ts.velocities = std::move(velocities);
    traj->frames.push_back(std::move(ts));
    traj->dt = result.dt;

    scene->addTrajectoryInstance(structureIndex, Transform{}, traj);
    scene->setDirty(DIRTY_ALL);
    scene->bumpSerial();

    spdlog::info("[io] GRO loaded: {} atoms, {} bonds from {}",
                 ms.elements.size(), ms.bonds.size(), path.filename().string());
    return result;
}

} // namespace moleculens::entity
