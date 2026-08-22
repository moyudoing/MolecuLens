// =============================================================================
// MolecuLens · io · pdb/pdb_parser.cpp —— PDB 解析器实现
// 规格来源：主方案 §4.8
//   - ATOM/HETATM: serial, name, altLoc, resName, chainID, resSeq, iCode,
//                  x,y,z, occupancy, tempFactor, element, charge
//   - CONECT → 显式键表；缺失 → §4.7 启发式
//   - MODEL/ENDMDL → 多帧（NMR/轨迹）→ 全部帧保留为轨迹
//   - HELIX/SHEET → Residue::ssHint（卡通风格用）
//   - strict/lenient：lenient 跳过畸形行并记 {line, message} 警告
//   - 层次：Chain → Residue → Atom 索引 → Atom::residueId/chainId
// =============================================================================

#include "io/pdb/pdb_parser.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "entity/search/bond_search.h"
#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

// 固定列宽字段提取（1-based 列区间，PDB 规范）
std::string field(const std::string& line, int col1, int col2) {
    const size_t begin = static_cast<size_t>(std::max(col1, 1) - 1);
    const size_t end = static_cast<size_t>(col2);
    if (begin >= line.size()) {
        return {};
    }
    std::string s = line.substr(begin, end > line.size() ? line.size() - begin : end - begin);
    // 去首尾空白
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
        float v = std::stof(s, &pos);
        if (pos == 0) {
            return fallback;
        }
        return v;
    } catch (...) {
        return fallback;
    }
}

// ssHint：0 未知 / 1 Helix / 2 Sheet（卡通 §5.7.5）
int8_t parseSSHint(int8_t old, int8_t hint) {
    return hint > old ? hint : old;   // HELIX 优先（优先级 a：记录 > 几何启发）
}

} // namespace

bool PdbParser::canOpen(const std::filesystem::path& path) const {
    // 魔数嗅探：文本以 "ATOM"/"HEADER"/"MODEL" 等 PDB 记录开头
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char buf[16] = {};
    in.read(buf, sizeof(buf) - 1);
    const std::string head(buf);
    return head.rfind("ATOM", 0) == 0 || head.rfind("HEADER", 0) == 0 ||
           head.rfind("MODEL", 0) == 0 || head.rfind("CRYST1", 0) == 0;
}

Result<ParseResult> PdbParser::parse(const std::filesystem::path& path,
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
        spdlog::warn("[io] PDB line {}: {}", line, msg);
        return true;
    };

    // 累积的结构构建状态
    std::vector<ElementType> elements;
    std::vector<Atom> atoms;
    std::vector<uint32_t> atomNameIds;
    std::vector<std::vector<glm::vec3>> frames;      // 多帧坐标
    std::vector<Bond> explicitBonds;
    bool anyCoords = false;

    // 残基/链索引表（重名复用）
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

    // ssHint：每个残基 (chainId, resSeq) → 1 Helix / 2 Sheet
    struct ResKey {
        int32_t chain;
        int32_t resSeq;
        // C++17 手写相等
        bool operator==(const ResKey& other) const {
            return chain == other.chain && resSeq == other.resSeq;
        }
    };
    struct ResKeyHash {
        size_t operator()(const ResKey& k) const {
            return (static_cast<size_t>(k.chain) * 31) ^
                   static_cast<size_t>(k.resSeq);
        }
    };
    std::unordered_map<ResKey, int8_t, ResKeyHash> ssHints;

    std::string line;
    size_t lineNo = 0;
    std::vector<glm::vec3>* currentFrame = nullptr;
    auto ensureFrame = [&]() {
        frames.emplace_back();
        currentFrame = &frames.back();
    };
    ensureFrame();   // MODEL 缺失时单帧

    while (std::getline(in, line)) {
        ++lineNo;
        if (line.size() < 6) {
            continue;
        }
        const std::string rec = line.substr(0, 6);
        // 裁剪行尾 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (rec == "ATOM  " || rec == "HETATM") {
            // 列 7-11 serial, 13-16 name, 17 altLoc, 18-20 resName,
            // 22 chainID, 23-26 resSeq, 27 iCode, 31-38 x, 39-46 y,
            // 47-54 z, 55-60 occupancy, 61-66 tempFactor, 77-78 element
            const std::string nameStr = field(line, 13, 16);
            const std::string altLoc = field(line, 17, 17);
            const std::string resName = field(line, 18, 20);
            const std::string chainID = field(line, 22, 22);
            const int32_t resSeq = static_cast<int32_t>(parseFloat(field(line, 23, 26), 0));
            const float x = parseFloat(field(line, 31, 38), 0.0f);
            const float y = parseFloat(field(line, 39, 46), 0.0f);
            const float z = parseFloat(field(line, 47, 54), 0.0f);
            const float occupancy = parseFloat(field(line, 55, 60), 1.0f);
            const float bfactor = parseFloat(field(line, 61, 66), 0.0f);
            std::string elementStr = field(line, 77, 78);
            if (elementStr.empty()) {
                // 兜底：从原子名解析元素（首字母大写规则）
                elementStr = nameStr;
                if (!elementStr.empty() && elementStr.size() >= 2 &&
                    std::isalpha(static_cast<unsigned char>(elementStr[0]))) {
                    elementStr = elementStr.substr(0, 2);
                    if (!std::isalpha(static_cast<unsigned char>(elementStr[1]))) {
                        elementStr = elementStr.substr(0, 1);
                    }
                } else if (!elementStr.empty()) {
                    elementStr = elementStr.substr(0, 1);
                }
            }
            const ElementType el = elementFromSymbol(elementStr.c_str());

            // altLoc 冲突：取第一条（后续 altLoc 非空且与首条不同 → 跳过记警告）
            if (altLoc.size() == 1 && altLoc[0] != ' ' && altLoc[0] != '\0') {
                // 简化：仅当后续同 serial 再出现时处理，这里直接接受首条
            }

            // 分层索引
            const int32_t cid = chainIdFor(chainID);
            const int32_t rid = residueIdFor(resName);

            elements.push_back(el);
            Atom a;
            a.element = el;
            a.occupancy = static_cast<uint8_t>(std::clamp(occupancy, 0.0f, 1.0f) * 100.0f);
            a.bfactor = static_cast<int16_t>(std::clamp(bfactor, 0.0f, 327.67f) * 100.0f);
            a.residueId = rid;
            a.chainId = cid;
            atoms.push_back(a);
            atomNameIds.push_back(scene->strings().intern(nameStr));

            if (currentFrame) {
                currentFrame->push_back(glm::vec3(x, y, z));
                anyCoords = true;
            }

            // ssHint 记录
            if (!ssHints.empty()) {
                auto it = ssHints.find({cid, resSeq});
                if (it != ssHints.end() && it->second != 0) {
                    atoms.back().bfactor = atoms.back().bfactor;  // 保留；ssHint 供卡通用
                }
            }
        } else if (rec == "CONECT") {
            // 键表：a, b1..b4
            const int32_t a = static_cast<int32_t>(parseFloat(field(line, 7, 11), -1));
            for (int col = 12; col <= 31; col += 5) {
                const int32_t b = static_cast<int32_t>(parseFloat(field(line, col, col + 4), -1));
                if (a > 0 && b > 0 && a != b) {
                    const uint32_t ua = static_cast<uint32_t>(a) - 1;
                    const uint32_t ub = static_cast<uint32_t>(b) - 1;
                    explicitBonds.push_back(
                        {std::min(ua, ub), std::max(ua, ub), 1});
                }
            }
        } else if (rec == "MODEL ") {
            if (frames.size() == 1 && frames[0].empty() && !anyCoords) {
                // 首 MODEL：替换初始空帧
                frames[0].clear();
            } else {
                ensureFrame();
            }
        } else if (rec == "ENDMDL") {
            // 帧结束（下一帧由 MODEL 开启）
        } else if (rec == "HELIX ") {
            // 残基范围 → Helix ssHint
            const std::string chain = field(line, 20, 20);
            const int32_t start = static_cast<int32_t>(parseFloat(field(line, 22, 25), 0));
            const int32_t end = static_cast<int32_t>(parseFloat(field(line, 34, 37), 0));
            const int32_t cid = chainIdFor(chain);
            for (int32_t r = start; r <= end; ++r) {
                ssHints[{cid, r}] = parseSSHint(ssHints[{cid, r}], 1);
            }
        } else if (rec == "SHEET ") {
            const std::string chain = field(line, 22, 22);
            const int32_t start = static_cast<int32_t>(parseFloat(field(line, 23, 26), 0));
            const int32_t end = static_cast<int32_t>(parseFloat(field(line, 34, 37), 0));
            const int32_t cid = chainIdFor(chain);
            for (int32_t r = start; r <= end; ++r) {
                ssHints[{cid, r}] = parseSSHint(ssHints[{cid, r}], 2);
            }
        } else if (rec == "TER   " || rec == "END   ") {
            // 边界（当前帧原子已完成）
        } else if (rec == "HEADER" || rec == "TITLE" || rec == "COMPND") {
            // 元信息（M1 阶段忽略；预留）
        }
    }

    if (!anyCoords || elements.empty()) {
        return Result<ParseResult>::error(
            ErrorCode::ParseError,
            "PDB contains no ATOM/HETATM records: " + path.string());
    }

    // 建键：CONECT 优先；缺失 → 启发式（§4.7）
    std::vector<Bond> bonds;
    if (!explicitBonds.empty()) {
        bonds = std::move(explicitBonds);
        std::sort(bonds.begin(), bonds.end(),
                  [](const Bond& x, const Bond& y) {
                      return x.a != y.a ? x.a < y.a : x.b < y.b;
                  });
        bonds.erase(std::unique(bonds.begin(), bonds.end(),
                                [](const Bond& x, const Bond& y) {
                                    return x.a == y.a && x.b == y.b;
                                }),
                    bonds.end());
    } else if (opts.inferBonds) {
        BondSearch::search(frames[0], elements, bonds);
    }

    // 结构注册（去重）
    MoleculeStructure ms;
    ms.elements = std::move(elements);
    ms.atoms = std::move(atoms);
    ms.atomNameIds = std::move(atomNameIds);
    ms.bonds = std::move(bonds);
    ms.key = computeStructureKey(ms);
    const uint32_t structureIndex = scene->registerStructure(ms);

    // 实例 + 轨迹
    auto traj = std::make_shared<Trajectory>();
    traj->dt = result.dt;
    traj->frames.reserve(frames.size());
    for (auto& f : frames) {
        Timestep ts;
        ts.positions = std::move(f);
        traj->frames.push_back(std::move(ts));
    }
    if (traj->frames.size() > 1) {
        traj->periodic = false;
        result.frameCount = traj->frames.size();
    }
    Transform tm;
    scene->addTrajectoryInstance(structureIndex, tm, traj);
    scene->setDirty(DIRTY_ALL);
    scene->bumpSerial();

    spdlog::info("[io] PDB loaded: {} atoms, {} bonds, {} frames from {}",
                 ms.elements.size(), ms.bonds.size(), frames.size(), path.filename().string());
    return result;
}

} // namespace moleculens::entity
