// =============================================================================
// MolecuLens v2 · app/workbench/analysis/molecule_stats.cpp
// 统计口径与采样策略见头文件；实现保持纯只读、与当前时间轴帧无关。
// =============================================================================

#include "app/workbench/analysis/molecule_stats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::app {

namespace {

// 轨迹抽样上限：打开分析面板/导出时都要在交互时间内完成。
// md.gro + md.xtc（2042 分子 / 6452 原子 / 1001 帧）实测 200 帧约几十毫秒；
// 再多也只是让"涨落统计"更平滑，收益远小于卡顿代价。
constexpr uint32_t kSampleFrames = 200;

glm::mat4 worldMatrixOf(const entity::SceneStructure& scene,
                        const entity::MoleculeInstance& inst) {
    glm::mat4 m = inst.transform.matrix();
    if (inst.systemId != UINT32_MAX) {
        const uint32_t si = scene.systemIndexById(inst.systemId);
        if (si != UINT32_MAX) {
            m = scene.systems()[si].transform.matrix() * m;
        }
    }
    return m;
}

// 取一帧里本实例的**局部**坐标，并按正交盒子做分子内最小镜像补齐
// （与渲染层 unwrap 同一口径；不做这一步时跨盒边界的分子会把 RMSE 抬到几十 Å）。
void frameLocalPositions(const entity::Timestep& ts,
                         const entity::MoleculeInstance& inst, size_t n,
                         std::vector<glm::vec3>& out) {
    out.resize(n, glm::vec3(0.0f));
    const float lx = ts.box[0];
    const float ly = ts.box[4];
    const float lz = ts.box[8];
    const bool pbc = lx > 1e-6f && ly > 1e-6f && lz > 1e-6f;
    glm::vec3 anchor(0.0f);
    for (size_t i = 0; i < n; ++i) {
        const size_t src = static_cast<size_t>(inst.trajAtomOffset) + i;
        glm::vec3 p = src < ts.positions.size() ? ts.positions[src] : glm::vec3(0.0f);
        if (i == 0) {
            anchor = p;
        } else if (pbc) {
            glm::vec3 d = p - anchor;
            d.x -= lx * std::round(d.x / lx);
            d.y -= ly * std::round(d.y / ly);
            d.z -= lz * std::round(d.z / lz);
            p = anchor + d;
        }
        out[i] = p;
    }
}

// 轨迹抽样：首帧 + 等间隔 + 末帧（保证长轨迹的末态也参与统计）。
std::vector<uint64_t> sampleFrames(uint64_t frames) {
    std::vector<uint64_t> out;
    if (frames == 0) {
        return out;
    }
    out.push_back(0);
    if (frames == 1) {
        return out;
    }
    const uint64_t cap = std::max<uint64_t>(2, kSampleFrames);
    const uint64_t stride = std::max<uint64_t>(1, (frames - 1) / (cap - 1));
    for (uint64_t f = stride; f < frames && out.size() < cap; f += stride) {
        out.push_back(f);
    }
    if (out.back() != frames - 1 && out.size() < cap) {
        out.push_back(frames - 1);
    }
    return out;
}

void accumulateFrame(const std::vector<glm::vec3>& world,
                     const glm::mat4& xform,
                     std::vector<glm::vec3>& worldOut) {
    worldOut.resize(world.size());
    for (size_t i = 0; i < world.size(); ++i) {
        worldOut[i] = glm::vec3(xform * glm::vec4(world[i], 1.0f));
    }
}

} // namespace

MoleculeStats computeMoleculeStats(const entity::SceneStructure& scene) {
    MoleculeStats result;
    const auto& instances = scene.instances();
    const auto& structures = scene.structures();
    const auto& positions = scene.coordsSoA().positions;
    const auto& systems = scene.systems();

    result.molecules.reserve(instances.size());
    for (uint32_t idx = 0; idx < instances.size(); ++idx) {
        const auto& inst = instances[idx];
        if (inst.structureIndex >= structures.size()) {
            continue;
        }
        const auto& structure = structures[inst.structureIndex];
        MoleculeStat st;
        st.instance = idx;
        st.name = inst.name;
        st.atoms = structure.elements.size();
        st.bonds = structure.bonds.size();
        if (inst.systemId != UINT32_MAX) {
            const uint32_t si = scene.systemIndexById(inst.systemId);
            if (si != UINT32_MAX) {
                st.system = systems[si].name;
            }
        }
        for (const auto el : structure.elements) {
            st.mass += static_cast<double>(entity::elementInfo(el).mass);
        }

        const glm::mat4 xform = worldMatrixOf(scene, inst);
        const entity::Trajectory* traj = inst.trajectory.get();
        st.frames = traj != nullptr ? traj->frames.size() : 0;
        const std::vector<uint64_t> sampled = sampleFrames(st.frames);

        // —— 首帧参考坐标（世界坐标）——
        // refLocal：首帧局部坐标（分子内已补齐，保证 Rg/形状正确）；
        // 后续每一帧都以它 + 逐原子**相对首帧**的最小镜像位移重建 —— 这样
        // "整帧锚原子换了一个镜像"不会把位移算进 RMSE（水分子在 40 Å 盒子里
        // 扩散时，错误做法会得到 ~28 Å 的假 RMSE）。
        std::vector<glm::vec3> refLocal;
        std::vector<glm::vec3> refWorld;
        if (traj != nullptr && !traj->frames.empty()) {
            frameLocalPositions(traj->frames[0], inst, st.atoms, refLocal);
            accumulateFrame(refLocal, xform, refWorld);
        } else if (st.atoms > 0) {
            const uint32_t base = scene.globalAtomBase(idx);
            refWorld.resize(st.atoms, glm::vec3(0.0f));
            for (size_t i = 0; i < st.atoms; ++i) {
                const size_t g = static_cast<size_t>(base) + i;
                refWorld[i] = g < positions.size() ? positions[g] : glm::vec3(0.0f);
            }
        }
        if (refWorld.empty()) {
            continue;
        }

        // —— 首帧几何：质心 / Rg / 包围盒 ——
        double weightSum = 0.0;
        glm::vec3 weighted(0.0f);
        std::vector<float> weights(st.atoms, 1.0f);
        if (st.atoms > 0 && st.mass > 0.0) {
            for (size_t i = 0; i < st.atoms; ++i) {
                const float m = entity::elementInfo(structure.elements[i]).mass;
                weights[i] = m > 0.0f ? m : 0.0f;
                weightSum += weights[i];
                weighted += refWorld[i] * weights[i];
            }
        }
        if (weightSum <= 0.0) {
            weightSum = static_cast<double>(st.atoms);
            weighted = glm::vec3(0.0f);
            for (size_t i = 0; i < st.atoms; ++i) {
                weights[i] = 1.0f;
                weighted += refWorld[i];
            }
        }
        st.centroid = weighted / static_cast<float>(weightSum);
        double rg2 = 0.0;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        for (size_t i = 0; i < st.atoms; ++i) {
            const glm::vec3 d = refWorld[i] - st.centroid;
            rg2 += static_cast<double>(weights[i]) * static_cast<double>(glm::dot(d, d));
            lo = glm::min(lo, refWorld[i]);
            hi = glm::max(hi, refWorld[i]);
        }
        st.radiusOfGyration =
            static_cast<float>(std::sqrt(rg2 / std::max(weightSum, 1e-9)));
        st.extent = hi - lo;

        // —— 轨迹涨落：RMSE（相对首帧）/ RMSF / 最大位移 ——
        st.framesUsed = static_cast<uint32_t>(sampled.size());
        if (sampled.size() > 1) {
            std::vector<glm::vec3> curLocal(st.atoms, glm::vec3(0.0f));
            std::vector<glm::vec3> curWorld;
            std::vector<glm::vec3> sumPos(st.atoms, glm::vec3(0.0f));
            std::vector<double> sumSq(st.atoms, 0.0);
            double disp2 = 0.0;
            float maxDisp = 0.0f;
            for (uint64_t fi : sampled) {
                if (fi >= traj->frames.size()) {
                    continue;
                }
                const auto& frame = traj->frames[fi];
                const float lx = frame.box[0];
                const float ly = frame.box[4];
                const float lz = frame.box[8];
                const bool pbc = lx > 1e-6f && ly > 1e-6f && lz > 1e-6f;
                for (size_t i = 0; i < st.atoms; ++i) {
                    const size_t src =
                        static_cast<size_t>(inst.trajAtomOffset) + i;
                    const glm::vec3 raw =
                        src < frame.positions.size() ? frame.positions[src]
                                                     : refLocal[i];
                    glm::vec3 d = raw - refLocal[i];
                    if (pbc) {
                        // 逐原子相对**首帧自身位置**做最小镜像：保持分子连续，
                        // 又不会把整体换镜像的那一步算成位移。
                        d.x -= lx * std::round(d.x / lx);
                        d.y -= ly * std::round(d.y / ly);
                        d.z -= lz * std::round(d.z / lz);
                    }
                    curLocal[i] = refLocal[i] + d;
                }
                accumulateFrame(curLocal, xform, curWorld);
                for (size_t i = 0; i < st.atoms; ++i) {
                    const glm::vec3 d = curWorld[i] - refWorld[i];
                    disp2 += static_cast<double>(glm::dot(d, d));
                    sumPos[i] += curWorld[i];
                    sumSq[i] += static_cast<double>(glm::dot(curWorld[i], curWorld[i]));
                    maxDisp = std::max(maxDisp, glm::length(d));
                }
            }
            const size_t used = static_cast<size_t>(st.framesUsed);
            const double count = static_cast<double>(used) * static_cast<double>(st.atoms);
            st.rmse = count > 0.0 ? static_cast<float>(std::sqrt(disp2 / count)) : 0.0f;
            double flucSum = 0.0;
            for (size_t i = 0; i < st.atoms; ++i) {
                const glm::vec3 mean = sumPos[i] / static_cast<float>(used);
                const double fluc = sumSq[i] -
                                    static_cast<double>(used) *
                                        static_cast<double>(glm::dot(mean, mean));
                flucSum += std::max(fluc, 0.0);
            }
            st.rmsf = count > 0.0
                          ? static_cast<float>(std::sqrt(flucSum / count))
                          : 0.0f;
            st.maxDisplacement = maxDisp;
        } else {
            st.framesUsed = st.frames > 0 ? 1u : 0u;
        }

        result.molecules.push_back(std::move(st));
    }

    // —— 汇总 ——
    auto& sum = result.summary;
    sum.molecules = result.molecules.size();
    sum.systems = systems.size();
    double rgSum = 0.0;
    double rmseSum = 0.0;
    double rmsfSum = 0.0;
    for (const auto& m : result.molecules) {
        sum.atoms += m.atoms;
        sum.bonds += m.bonds;
        sum.maxFrames = std::max(sum.maxFrames, m.frames);
        sum.totalMass += m.mass;
        rgSum += m.radiusOfGyration;
        rmseSum += m.rmse;
        rmsfSum += m.rmsf;
        sum.maxRadiusOfGyration =
            std::max(sum.maxRadiusOfGyration, m.radiusOfGyration);
        if (m.frames > 1) {
            ++sum.withTrajectory;   // 单帧只是静态结构的承载，不算"有轨迹"
        }
        if (m.rmse > sum.maxRmse) {
            sum.maxRmse = m.rmse;
            sum.maxRmseName = m.name;
        }
    }
    if (sum.molecules > 0) {
        const float n = static_cast<float>(sum.molecules);
        sum.meanAtoms = static_cast<float>(sum.atoms) / n;
        sum.meanBonds = static_cast<float>(sum.bonds) / n;
        sum.meanRadiusOfGyration = static_cast<float>(rgSum / n);
        sum.meanRmse = static_cast<float>(rmseSum / n);
        sum.meanRmsf = static_cast<float>(rmsfSum / n);
    }
    return result;
}

std::vector<std::string> moleculeStatsCsvHeader() {
    return {
        "序号",          "分子",          "体系",            "原子数",
        "键数",          "总帧数",        "统计帧数",        "质量(u)",
        "质心X(Å)",      "质心Y(Å)",      "质心Z(Å)",        "回转半径Rg(Å)",
        "尺寸X(Å)",      "尺寸Y(Å)",      "尺寸Z(Å)",        "RMSE(Å)",
        "RMSF(Å)",       "最大位移(Å)",
    };
}

std::vector<std::vector<std::string>> moleculeStatsCsvRows(
    const MoleculeStats& stats) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(stats.molecules.size());
    char buf[64];
    for (size_t i = 0; i < stats.molecules.size(); ++i) {
        const auto& m = stats.molecules[i];
        std::vector<std::string> row;
        row.reserve(18);
        std::snprintf(buf, sizeof(buf), "%zu", i + 1);
        row.emplace_back(buf);
        row.push_back(m.name);
        row.push_back(m.system);
        std::snprintf(buf, sizeof(buf), "%zu", m.atoms);
        row.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "%zu", m.bonds);
        row.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(m.frames));
        row.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "%u", m.framesUsed);
        row.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "%.3f", m.mass);
        row.emplace_back(buf);
        const float centroid[3] = {m.centroid.x, m.centroid.y, m.centroid.z};
        for (float v : centroid) {
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            row.emplace_back(buf);
        }
        std::snprintf(buf, sizeof(buf), "%.4f", m.radiusOfGyration);
        row.emplace_back(buf);
        const float extent[3] = {m.extent.x, m.extent.y, m.extent.z};
        for (float v : extent) {
            std::snprintf(buf, sizeof(buf), "%.4f", v);
            row.emplace_back(buf);
        }
        std::snprintf(buf, sizeof(buf), "%.4f", m.rmse);
        row.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "%.4f", m.rmsf);
        row.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "%.4f", m.maxDisplacement);
        row.emplace_back(buf);
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace moleculens::app
