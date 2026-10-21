// =============================================================================
// Moleculens · engine · style/backbone/backbone_trace.h —— CA 骨架路径提取
// 用途：Tube / Ribbon 等"仅 backbone"样式共用同一份 CA 提取（此前 tube.cpp 与
//       ribbon.cpp 各抄了一份逐字相同的 Key/ResidueCA 结构与分组循环）。
//
// 关键点：CA 点必须随点携带其**全局原子下标**。
//   配色（Representer::colorForAtom）与拾取（pickId_pad/pickId）两者都以全局原子
//   下标为键。此前两个样式传的是"链内序号"（0,1,2,…），于是：
//     · 元素配色取的是原子 0/1/2… 的元素，与真正画出来的 CA 无关（整条管/带
//       的颜色取决于结构里前几个原子是什么元素）；
//     · 点选管/带命中的是别的原子（交互错位）。
//   渲染外观完全正常，只有颜色与交互是错的 —— 属于不会当场暴露的缺陷。
//
// 与 mask 的关系：本函数**不做** mask 过滤。过滤掉中间的点会让断口两侧直接
// 连成一条并不存在的直线；正确做法是保留完整路径，由调用方按"段两端都在选区内"
// 决定是否发射该段几何（见 drawTube / drawRibbon）。
// =============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/rep/representer.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

// 一个 CA 采样点：坐标 + 全局原子下标（配色/拾取的键）
struct CaPoint {
    glm::vec3 p{0.0f};
    uint32_t  globalAtom = 0;
};

// 按 (chainId, residueId) 提取 CA 并**按链分组**（残基顺序由 residueId 保证，
// 与 cartoon 的骨架提取同法）。缺 CA 的残基不产生点，自然形成断口。
// pos 由调用方传入（已确认非空），避免在循环内重复取全局坐标数组。
inline void extractCaChains(const RepContext& ctx,
                            const entity::MoleculeStructure& st,
                            const glm::vec3* pos,
                            std::vector<std::vector<CaPoint>>& outChains) {
    struct Key {
        int32_t chain;
        int32_t res;
        bool operator<(const Key& o) const {
            return chain != o.chain ? chain < o.chain : res < o.res;
        }
    };

    std::map<Key, CaPoint> byRes;
    const size_t n = st.elements.size();
    for (size_t i = 0; i < n; ++i) {
        // 原子名可能左/右填充（"CA"/" CA"/"CA "）——与 cartoon 判定一致
        const std::string_view name =
            ctx.scene->atomName(ctx.instance->structureIndex,
                                static_cast<uint32_t>(i));
        if (name != "CA" && name != "CA ") {
            continue;
        }
        CaPoint cp;
        cp.globalAtom = ctx.globalAtomBase + static_cast<uint32_t>(i);
        cp.p = pos[cp.globalAtom];
        byRes[{st.atoms[i].chainId, st.atoms[i].residueId}] = cp;
    }

    outChains.clear();
    int32_t curChain = INT32_MIN;
    for (const auto& [k, cp] : byRes) {
        if (k.chain != curChain) {
            outChains.emplace_back();
            curChain = k.chain;
        }
        outChains.back().push_back(cp);
    }
}

// 段 [a, b] 是否可绘制：两端原子都在选择掩码内。
// 与 stick/licorice 对"键"的处理语义一致（键任一端未选中即不画）。
inline bool caSegmentSelected(const RepContext& ctx, const CaPoint& a,
                              const CaPoint& b) {
    if (ctx.mask == nullptr) {
        return true;
    }
    const size_t n = ctx.mask->size();
    return a.globalAtom < n && b.globalAtom < n && (*ctx.mask)[a.globalAtom] &&
           (*ctx.mask)[b.globalAtom];
}

// 断链阈值（Å）：相邻 CA 距离超过此值视为链断裂，不连接（主方案 §5.7.5 同值）
inline constexpr float kCaBreakThreshold = 4.2f;

} // namespace moleculens::engine
