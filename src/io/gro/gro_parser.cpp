// =============================================================================
// MolecuLens · io · gro/gro_parser.cpp —— GRO 解析器实现
// 规格来源：主方案 §4.8
//   GRO 格式：第 1 行标题；第 2 行原子数；后续每原子一行固定列宽：
//     resnr(5) resname(5) atomname(5) atomnr(5) x(8.3) y(8.3) z(8.3) [vx vy vz]
//   末行 box（3 或 9 浮点，nm）
//   单位：nm → Å（×10），units 记录 Angstrom（统一换算）
// 大体系支持（溶剂化体系，如 data/md.gro 6452 原子）
//   - 读入盒子（末行 3/9 浮点 nm）：存 Timestep.box[9]（对角线 ×10Å）+ periodic
//   - 分子分离：按 (resnr, resname) 连续块拆为一个分子实例（registerStructure
//     去重，同构水分子共享拓扑），每个分子只做分子内键推断（含 PBC 最小镜像
//     BondSearch），消除跨溶剂/跨分子的假键
//   - 溶剂标记：残基名 ∈ 常见水/离子 → MoleculeInstance.isSolvent=true（可整体显隐）
// =============================================================================

#include "io/gro/gro_parser.h"

#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

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

// GRO 原子名 → 元素类型（正确距离判键的前置：元素必须解析对）。
// GRO 常见命名不是元素符号：水 OW/HW、离子 NA/CL、残基原子 C05/O01（数字序号）。
// 规则（参考 OpenBabel guessbonds 的元素推断）：
//   1) 去掉尾随数字与常见英文标记字符（'W','H','1','2' 等）后精确匹配元素符号
//   2) 否则贪心取最长可转元素的前缀（<=2 字母），剩余视为标记（OW→O、HW→H）
//   3) 仍失败 → Unknown（键搜索对 Unknown 原子跳过，避免 0 半径假键）
ElementType elementFromGroName(const std::string& atomName) {
    // 去尾随数字（C05 → C05 先留，再试前缀）
    std::string base = atomName;
    while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) {
        base.pop_back();
    }
    // 1) 精确匹配（CL → Cl、NA → Na、MG → Mg）
    if (ElementType e = elementFromSymbol(base.c_str()); e != ElementType::Unknown) {
        return e;
    }
    // 2) 常见水标记：去掉尾字母 'W'（OW→O、HW→H）
    if (base.size() >= 2 && (base.back() == 'W' || base.back() == 'w')) {
        const std::string p = base.substr(0, base.size() - 1);
        if (ElementType e = elementFromSymbol(p.c_str()); e != ElementType::Unknown) {
            return e;
        }
    }
    // 3) 贪心 2 字母前缀（OW→O、HW→H、SW→S），再试 1 字母
    for (int len = 2; len >= 1; --len) {
        if (base.size() < static_cast<size_t>(len)) {
            continue;
        }
        const std::string p = base.substr(0, static_cast<size_t>(len));
        if (ElementType e = elementFromSymbol(p.c_str()); e != ElementType::Unknown) {
            return e;
        }
    }
    return ElementType::Unknown;
}

// 残基名 → 溶剂判定（水/离子：TIPS/SPC 水、W、SOL、HOH 及常见离子的 resname 缩写）
bool isSolventResidue(const std::string& res) {
    static const char* kSolventNames[] = {
        "SOL", "HOH", "WAT", "W", "TIP3", "TIP4", "TIP5", "SPC", "SPCE", "SPC/E",
        "CL ", "CLA", "NA+", "K+", "NA", "K", "Mg", "Ca", "Cl-", "CL ",
    };
    for (const char* n : kSolventNames) {
        if (res == n) {
            return true;
        }
    }
    return false;
}

// 逐行原子记录（解析时暂存，按分子分组组装）
struct GroAtomLine {
    std::string resName;
    std::string atomName;
    glm::vec3   pos;      // Å
    glm::vec3   vel;      // Å（可选）
    bool        hasVel = false;
};

// 组装一个分子（连续同残基原子）→ 结构 + 实例（含 PBC 键、溶剂标记）
struct MoleculeAcc {
    std::vector<GroAtomLine> atoms;
    std::string              resName;
    bool                     isSolvent = false;
};

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

    // 残基名/链名去重表（按残基名做 chain，与溶剂分组一致）
    std::unordered_map<std::string, int32_t> chainMap, residueMap;
    auto chainIdFor = [&](const std::string& name) -> int32_t {
        auto it = chainMap.find(name);
        if (it != chainMap.end()) {
            return it->second;
        }
        const int32_t id = scene->addChain(name.empty() ? " " : name);
        chainMap.emplace(name, id);
        return id;
    };
    // 残基按"每分子一个实例"分配：GRO 没有真残基概念，若按名字去重，
    // md.gro 的 2032 个 SOL 会共用同一个 residueId → "按残基"配色整片同色。
    auto residueIdForInstance = [&](const std::string& key, const std::string& name) -> int32_t {
        auto it = residueMap.find(key);
        if (it != residueMap.end()) {
            return it->second;
        }
        const int32_t id = scene->addResidue(name.empty() ? " " : name);
        residueMap.emplace(key, id);
        return id;
    };

    // 逐分子分组（同 resnr+resname 连续块 = 一个分子）
    std::vector<MoleculeAcc> molecules;
    {
        std::string curKey;
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
            const std::string resNr = field(line, 1, 5);
            const std::string resName = field(line, 6, 10);
            const std::string atomName = field(line, 11, 15);
            const float xNm = parseFloat(field(line, 21, 28), 0.0f);
            const float yNm = parseFloat(field(line, 29, 36), 0.0f);
            const float zNm = parseFloat(field(line, 37, 44), 0.0f);
            const bool hasVel = line.size() >= 68;

            GroAtomLine rec;
            rec.resName = resName;
            rec.atomName = atomName;
            rec.pos = glm::vec3(xNm * 10.0f, yNm * 10.0f, zNm * 10.0f);   // nm→Å
            rec.hasVel = hasVel;
            if (hasVel) {
                rec.vel = glm::vec3(parseFloat(field(line, 45, 52), 0.0f) * 10.0f,
                                    parseFloat(field(line, 53, 60), 0.0f) * 10.0f,
                                    parseFloat(field(line, 61, 68), 0.0f) * 10.0f);
            }

            // 新分子：残基号/名变化即开启新块（GRO 中连续同残基 = 同一分子）
            const std::string key = resNr + "|" + resName;
            if (molecules.empty() || key != curKey) {
                molecules.push_back(MoleculeAcc{});
                molecules.back().resName = resName;
                molecules.back().isSolvent = isSolventResidue(resName);
                curKey = key;
            }
            molecules.back().atoms.push_back(std::move(rec));
        }
    }

    if (molecules.empty()) {
        return Result<ParseResult>::error(ErrorCode::ParseError,
                                          "GRO contains no atoms: " + path.string());
    }

    // 末行：盒子（3 或 9 浮点，nm）——当前已读取到行尾；重开文件读最后一行
    std::array<float, 9> box{};   // Å（对角线 ×10；完整矩阵 9 元原样 ×10）
    bool periodic = false;
    {
        std::ifstream tail(path, std::ios::binary);
        std::string last;
        std::string l;
        while (std::getline(tail, l)) {
            if (!l.empty() && l.back() == '\r') {
                l.pop_back();
            }
            last = l;
        }
        if (!last.empty()) {
            std::istringstream ss(last);
            std::vector<float> v;
            float f;
            while (ss >> f) {
                v.push_back(f);
            }
            if (!v.empty()) {
                periodic = true;   // gro 盒子行存在即周期
                // 用前 3 个非零长轴；对角 ×10 Å；9 元则逐轴取 v[0],v[4],v[8]
                box[0] = (v.size() >= 9 && v[0] != 0.0f) ? v[0] : (v.size() >= 1 ? v[0] : 0.0f);
                box[4] = (v.size() >= 9 && v[4] != 0.0f) ? v[4] : (v.size() >= 2 ? v[1] : 0.0f);
                box[8] = (v.size() >= 9 && v[8] != 0.0f) ? v[8] : (v.size() >= 3 ? v[2] : 0.0f);
                box[0] *= 10.0f;
                box[4] *= 10.0f;
                box[8] *= 10.0f;
            }
        }
    }
    const float pbc[3] = {box[0], box[4], box[8]};
    const bool usePbc = periodic && pbc[0] > 0.0f && pbc[1] > 0.0f && pbc[2] > 0.0f;

    // 逐分子 → 结构（去重）+ 实例（每分子一个，含各自键）
    int solventCount = 0;
    int moleculeCount = 0;
    for (auto& mol : molecules) {
        const size_t n = mol.atoms.size();
        if (n == 0) {
            continue;
        }
        std::vector<ElementType> elements;
        std::vector<Atom> atoms;
        std::vector<uint32_t> atomNameIds;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> velocities;
        const int32_t rid = residueIdForInstance(
            mol.resName + "#" + std::to_string(moleculeCount), mol.resName);
        const int32_t cid = chainIdFor(mol.resName);
        for (const auto& rec : mol.atoms) {
            // 元素解析（GRO 原子名 → 元素）：正确距离判键的前提。GRO 名如 OW/HW/NA/CL/C05
            // 不是元素符号，需前缀解析（见 elementFromGroName 注释）。
            const ElementType el = elementFromGroName(rec.atomName);
            elements.push_back(el);
            Atom a;
            a.element = el;
            a.occupancy = 100;
            a.bfactor = 0;
            a.residueId = rid;
            a.chainId = cid;
            atoms.push_back(a);
            atomNameIds.push_back(scene->strings().intern(rec.atomName));
            positions.push_back(rec.pos);
            if (rec.hasVel) {
                velocities.push_back(rec.vel);
            }
        }

        // 补齐跨盒子边界的分子（pbc mol / unwrap）：判键用最小镜像是正确的，但
        // **渲染**按原始坐标连键 —— 被拆开的分子会画成一根接近盒长的长棍（实测
        // md.gro 有 20 根、盒长 40.28 Å）。必须在判键与渲染之前把分子拉成连续几何。
        if (usePbc) {
            unwrapAroundFirst(positions.data(), positions.size(), box);
        }

        // 分子内键（PBC 最小镜像，仅本分子原子，无跨分子假键）
        std::vector<Bond> bonds;
        if (opts.inferBonds) {
            BondSearchOptions bo;
            bo.periodicBox = usePbc ? pbc : nullptr;
            BondSearch::search(positions, elements, bonds, bo);
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
        ts.box = box;
        traj->frames.push_back(std::move(ts));
        traj->dt = result.dt;
        traj->periodic = periodic;

        // 一次导入 = 一个体系（UINT32_MAX 挂当前体系）；每分子一实例
        // 名称 = 残基名 + 序号（分子列表可辨识）；溶剂标记直传 addTrajectoryInstance
        const std::string instName =
            mol.resName.empty() ? ("mol# " + std::to_string(moleculeCount + 1))
                                : (mol.resName + " #" + std::to_string(moleculeCount + 1));
        scene->addTrajectoryInstance(structureIndex, Transform{}, traj, UINT32_MAX,
                                     instName, mol.isSolvent);
        if (mol.isSolvent) {
            ++solventCount;
        }
        ++moleculeCount;
    }

    if (moleculeCount == 0) {
        return Result<ParseResult>::error(ErrorCode::ParseError,
                                          "GRO contains no molecules: " + path.string());
    }

    scene->setDirty(DIRTY_ALL);
    scene->bumpSerial();

    // 原子/键总数必须按**实例**累加，不能遍历 structures()：结构池按 StructureKey
    // 去重，md.gro 的 2032 个水分子只共享一个结构，遍历池子会报成 210 原子
    // （实际 6452），日志与文件严重不符、误导排查。
    size_t totalAtoms = 0;
    size_t totalBonds = 0;
    for (const auto& inst : scene->instances()) {
        if (inst.structureIndex >= scene->structures().size()) {
            continue;
        }
        const auto& ms = scene->structures()[inst.structureIndex];
        totalAtoms += ms.elements.size();
        totalBonds += ms.bonds.size();
    }
    spdlog::info("[io] GRO loaded: {} molecules ({} solvent), {} atoms, {} bonds from {}",
                 moleculeCount, solventCount, totalAtoms, totalBonds,
                 path.filename().string());
    return result;
}

} // namespace moleculens::entity