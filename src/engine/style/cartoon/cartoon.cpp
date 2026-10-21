// =============================================================================
// MolecuLens · engine · style/cartoon/cartoon.cpp —— 卡通 6 步实现
// 规格来源：主方案 §5.7.5（① 骨架提取 ② SS 判定 ③ Catmull-Rom 样条
//           ④ 平行传输标架 ⑤ 几何挤出 ⑥ 三角化 → TriMesh）
// 简化注记：N/CA/C 坐标从原子名（"N"/"CA"/"C"）按残基提取。
// =============================================================================

#include "engine/style/cartoon/cartoon.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

namespace {

enum class SS : uint8_t { Helix, Sheet, Coil };

struct ResidueCoord {
    glm::vec3 n{0.0f};
    glm::vec3 ca{0.0f};
    glm::vec3 c{0.0f};
    bool hasN = false, hasCA = false, hasC = false;
};

// 链残基提取：按 chainId 分组，残基顺序由 residueId 保证（解析器递增）
void extractBackbones(const RepContext& ctx, const entity::MoleculeStructure& st,
                      std::vector<std::vector<ResidueCoord>>& outChains) {
    // (chainId, residueId) → ResidueCoord
    struct Key {
        int32_t chain;
        int32_t res;
        bool operator<(const Key& o) const {
            return chain != o.chain ? chain < o.chain : res < o.res;
        }
    };
    std::map<Key, ResidueCoord> byRes;
    const size_t n = st.elements.size();
    for (size_t i = 0; i < n; ++i) {
        const std::string_view name = ctx.scene->atomName(ctx.instance->structureIndex,
                                                          static_cast<uint32_t>(i));
        const int32_t cid = st.atoms[i].chainId;
        const int32_t rid = st.atoms[i].residueId;
        const glm::vec3* pos = Representer::globalPositions(ctx);
        if (!pos) {
            return;
        }
        const glm::vec3 p = pos[ctx.globalAtomBase + static_cast<uint32_t>(i)];
        auto& rc = byRes[{cid, rid}];
        if (name == "N" || name == " N") { rc.n = p; rc.hasN = true; }
        else if (name == "CA" || name == "CA ") { rc.ca = p; rc.hasCA = true; }
        else if (name == "C" || name == " C") { rc.c = p; rc.hasC = true; }
    }
    // 分组
    outChains.clear();
    int32_t curChain = INT32_MIN;
    for (const auto& [k, rc] : byRes) {
        if (k.chain != curChain) {
            outChains.emplace_back();
            curChain = k.chain;
        }
        outChains.back().push_back(rc);
    }
}

// 二面角（标准定义）
float dihedral(const glm::vec3& a, const glm::vec3& b,
               const glm::vec3& c, const glm::vec3& d) {
    const glm::vec3 b1 = b - a, b2 = c - b, b3 = d - c;
    const glm::vec3 c1 = glm::cross(b1, b2);
    const glm::vec3 c2 = glm::cross(b2, b3);
    const glm::vec3 c3 = glm::cross(c1, c2);
    const float len2 = glm::dot(c1, c1);
    const float ang = std::atan2(glm::dot(c3, b2) / std::sqrt(len2 + 1e-12f),
                                 glm::dot(c1, c2) / (std::sqrt(len2) + 1e-12f));
    return ang * 180.0f / 3.14159265358979f;
}

// Catmull-Rom 样条采样（4 控制点 → t∈[0,1] 内的点）
glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1,
                     const glm::vec3& p2, const glm::vec3& p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// 平行传输标架（主方案 §5.7.5 ④）
struct Frame {
    glm::vec3 T, N, B;
};

} // namespace

bool calcPhiPsi(const glm::vec3& prevN, const glm::vec3& ca,
                const glm::vec3& c, const glm::vec3& nextN,
                float* outPhi, float* outPsi) {
    if (outPhi) {
        *outPhi = dihedral(prevN, ca, c, nextN);
    }
    if (outPsi) {
        *outPsi = dihedral(ca, c, nextN, ca);  // 占位：psi 需下一残基 CA
    }
    return true;
}

void drawCartoon(const RepContext& ctx, SceneCommandList& out) {
    const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
    const glm::vec3* pos = Representer::globalPositions(ctx);
    if (!pos) {
        return;
    }

    // ① 骨架提取（按链/残基；缺原子残基跳过；断链重置）
    std::vector<std::vector<ResidueCoord>> chains;
    extractBackbones(ctx, st, chains);

    const CartoonConstants K;

    for (auto& chain : chains) {
        // ② 二级结构判定（几何启发；HELIX/SHEET 记录优先级在解析期写入，
        //    简化：此处纯几何判定）
        std::vector<SS> ss(chain.size(), SS::Coil);
        for (size_t i = 1; i + 1 < chain.size(); ++i) {
            const auto& prev = chain[i - 1];
            const auto& cur = chain[i];
            const auto& next = chain[i + 1];
            if (!prev.hasC || !cur.hasN || !cur.hasCA || !cur.hasC || !next.hasN) {
                continue;
            }
            // 标准二面角定义（此前两式都取错了原子，导致 1UBQ 全部判成 Coil）：
            //   φ(i) = dihedral(C(i-1), N(i), CA(i), C(i))
            //   ψ(i) = dihedral(N(i), CA(i), C(i), N(i+1))
            const float phi = dihedral(prev.c, cur.n, cur.ca, cur.c);
            const float psi = dihedral(cur.n, cur.ca, cur.c, next.n);
            if (phi >= K.phiHelixLo && phi <= K.phiHelixHi &&
                psi >= K.psiHelixLo && psi <= K.psiHelixHi) {
                ss[i] = SS::Helix;
            } else if (phi >= K.phiSheetLo && phi <= K.phiSheetHi &&
                       psi >= K.psiSheetLo && psi <= K.psiSheetHi) {
                ss[i] = SS::Sheet;
            }
        }

        // ③+④ CA 样条 + 平行传输标架
        struct Sample {
            glm::vec3 p, T, N, B;
            SS type = SS::Coil;    // 该采样点归属残基的二级结构（采样时确定，不再回推）
            uint32_t run = 0;      // 连续段号：断链/缺原子处 +1，用于切断桥接
        };
        std::vector<Sample> samples;
        uint32_t run = 0;
        bool havePrevSample = false;   // 上一段是否已有采样点（决定续接标架还是重开）
        for (size_t i = 0; i + 1 < chain.size(); ++i) {
            const auto& r0 = chain[i];
            const auto& r1 = chain[i + 1];
            if (!r0.hasCA || !r1.hasCA ||
                glm::length(r1.ca - r0.ca) > K.breakThreshold) {
                // 断链/缺失：结束当前连续段。下一段不得与上一段桥接（否则会连出
                // 一条跨越断口的假管子），标架也从默认值重新开始。
                ++run;
                havePrevSample = false;
                continue;
            }
            const glm::vec3 p0 = r0.ca, p1 = r1.ca;
            const glm::vec3 pm = (i > 0 && chain[i - 1].hasCA)
                                     ? chain[i - 1].ca
                                     : r0.ca - (r1.ca - r0.ca);
            const glm::vec3 p2 = (i + 2 < chain.size() && chain[i + 2].hasCA)
                                     ? chain[i + 2].ca
                                     : r1.ca + (r1.ca - r0.ca);
            // 平行传输标架跨段连续：平移后的 N 从上一段末采样继承
            glm::vec3 prevN = havePrevSample ? samples.back().N : glm::vec3(0, 0, 1);
            for (int k = 0; k < K.samplesPerSegment; ++k) {
                const float t = static_cast<float>(k) / K.samplesPerSegment;
                const glm::vec3 p = catmullRom(pm, p0, p1, p2, t);
                Sample s;
                s.p = p;
                // 平行传输：T = 切线；N 由上一帧投影正交化；B = T×N
                const glm::vec3 rawT =
                    catmullRom(pm, p0, p1, p2, std::min(1.0f, t + 1e-4f)) - p;
                const float tLen = glm::length(rawT);
                const glm::vec3 T =
                    tLen > 1e-8f ? rawT / tLen
                                 : (havePrevSample ? samples.back().T : glm::vec3(0, 0, 1));
                s.T = T;
                glm::vec3 N = prevN - T * glm::dot(prevN, T);
                if (glm::dot(N, N) < 1e-8f) {
                    // 退化：参考轴与切线平行 → 换一个不平行轴重建 N
                    const glm::vec3 axis =
                        std::abs(T.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                    N = axis - T * glm::dot(axis, T);
                }
                N = glm::normalize(N);
                s.N = N;
                s.B = glm::normalize(glm::cross(T, N));
                prevN = N;
                s.type = ss[std::min(i + 1, ss.size() - 1)];
                s.run = run;
                samples.push_back(s);
            }
            havePrevSample = true;
        }

        // ⑤ 几何挤出（按 SS 类型：Coil 管 / Helix 带 / Sheet 带+箭头）
        // ⑥ 三角化 → TriMesh
        struct Vtx {
            glm::vec3 p, nrm;
            glm::vec4 col;
        };
        std::vector<Vtx> verts;
        std::vector<uint32_t> indices;
        // 三角形带构建：记录上一环的**实际**基址与环顶点数。
        // 历史缺陷：用“当前环顶点数”反推上一环基址 —— Coil(8 顶点) 与
        // Helix/Sheet(2 顶点) 相邻时上一环基址算错，且桥接按当前环大小循环，
        // 会在二级结构切换处生成穿模三角形。改为显式记录并用取模索引按
        // max(prev, cur) 桥接，两种环大小可平滑过渡。
        uint32_t prevRingBase = 0;
        uint32_t prevRingSize = 0;
        uint32_t prevRun = UINT32_MAX;
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto& s = samples[i];
            const SS type = s.type;
            glm::vec4 col;
            if (type == SS::Helix) {
                col = glm::vec4(K.helixColor[0], K.helixColor[1], K.helixColor[2], 1.0f);
            } else if (type == SS::Sheet) {
                col = glm::vec4(K.sheetColor[0], K.sheetColor[1], K.sheetColor[2], 1.0f);
            } else {
                col = glm::vec4(K.coilColor[0], K.coilColor[1], K.coilColor[2], 1.0f);
            }

            // 环截面（管或带）
            std::vector<glm::vec3> ring;
            glm::vec3 ringNormal = s.B;   // 带：面法线 = 副法线；管：逐点径向
            if (type == SS::Coil) {
                for (int k = 0; k < K.tubeSegments; ++k) {
                    const float a = 6.2831853f * k / K.tubeSegments;
                    const float ca = std::cos(a), sa = std::sin(a);
                    ring.push_back(s.p + s.N * (ca * K.coilRadius) +
                                   s.B * (sa * K.coilRadius));
                }
            } else {
                // 带：宽 = Helix 1.6 / Sheet 渐变
                float width = (type == SS::Helix) ? K.helixWidth : K.sheetStartWidth;
                // Sheet 箭头：后半段渐变到 2.4
                const float prog = static_cast<float>(i) / std::max(1.0f,
                                                                    static_cast<float>(samples.size() - 1));
                if (type == SS::Sheet) {
                    width = prog > 0.5f
                                ? glm::mix(K.sheetStartWidth, K.sheetEndWidth,
                                           (prog - 0.5f) * 2.0f)
                                : K.sheetStartWidth;
                }
                ring.push_back(s.p + s.N * width * 0.5f);
                ring.push_back(s.p - s.N * width * 0.5f);
            }

            const uint32_t ringBase = static_cast<uint32_t>(verts.size());
            for (const auto& rp : ring) {
                Vtx v;
                v.p = rp;
                // 带是平面条带：法线用副法线（此前用径向 → 条带边缘受光，看起来
                // 像一根细棍而不是带）；管用径向法线。
                v.nrm = (type == SS::Coil) ? glm::normalize(rp - s.p) : ringNormal;
                v.col = col;
                verts.push_back(v);
            }
            // 仅在**同一连续段**内桥接（断链处不连）
            if (i > 0 && s.run == prevRun && prevRingSize > 0 && !ring.empty()) {
                const uint32_t nPrev = prevRingSize;
                const uint32_t nCur = static_cast<uint32_t>(ring.size());
                const uint32_t n = std::max(nPrev, nCur);
                for (uint32_t k = 0; k < n; ++k) {
                    const uint32_t k1 = k + 1;
                    const uint32_t pa = prevRingBase + k % nPrev;
                    const uint32_t pb = prevRingBase + k1 % nPrev;
                    const uint32_t ca = ringBase + k % nCur;
                    const uint32_t cb = ringBase + k1 % nCur;
                    indices.push_back(pa);
                    indices.push_back(ca);
                    indices.push_back(pb);
                    indices.push_back(pb);
                    indices.push_back(ca);
                    indices.push_back(cb);
                }
            }
            prevRingBase = ringBase;
            prevRingSize = static_cast<uint32_t>(ring.size());
            prevRun = s.run;
        }

        // TriMesh 命令（每链一个，主方案 §5.7.8）。
        // 经 appendTriMesh 发射：大体系（长链）单链网格也可能超单命令载荷上限，
        // 超限会让命令流在解码时被静默截断（整条链不可见）。
        if (verts.empty() || indices.empty()) {
            continue;
        }
        std::vector<TriMeshVertex> mesh;
        mesh.resize(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) {
            mesh[i].pos = glm::vec4(verts[i].p, 0.0f);
            mesh[i].normal = glm::vec4(verts[i].nrm, 0.0f);
            mesh[i].color = verts[i].col;
        }
        out.appendTriMesh(mesh, indices);
    }
}

} // namespace moleculens::engine
