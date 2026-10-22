// =============================================================================
// MolecuLens · engine · style/wireframe/wireframe.cpp —— 线状实现
// 规格来源：主方案 §5.7.4
// =============================================================================

#include "engine/style/wireframe/wireframe.h"

#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

    void drawWireframe(const RepContext& ctx, SceneCommandList& out) {

        // 取出当前实例对应的分子结构数据
        const auto& st = ctx.scene->structures()[ctx.instance->structureIndex];
        const size_t n = st.elements.size();

        // 获取全局原子坐标数组指针
        const glm::vec3* pos = Representer::globalPositions(ctx);
        // 坐标为空直接退出，防御性判空
        if (!pos) {
            return;
        }

        // 遍历该结构下全部化学键 bond
        for (const entity::Bond& bnd : st.bonds) {
            // 边界检查：原子索引越界，跳过这条键
            if (bnd.a >= n || bnd.b >= n) {
                continue;
            }
            // 转换为全局原子ID：当前实例的原子基偏移 + 局部原子索引
            const uint32_t ga = ctx.globalAtomBase + bnd.a;
            const uint32_t gb = ctx.globalAtomBase + bnd.b;

            // ==========掩码过滤核心逻辑==========
            // 1.全局原子ID不能超出mask数组大小
            // 2.mask标记为true代表该原子可见；两个原子都可见，才绘制这条化学键
            if (ga >= ctx.mask->size() || gb >= ctx.mask->size() ||
                !(*ctx.mask)[ga] || !(*ctx.mask)[gb]) {
                continue;
                }

            // 半键着色（Lines 的做法）：一条键拆成两段，各用本端原子的颜色。
            // 以前整条键用 a 端颜色 —— 水分子 O-H 全画成红色、蛋白质里 C-N 全按碳色，
            // 视觉上就像"一条链只有一种颜色"。拆半后按原子名/元素配色才分得开。
            const glm::vec3 pa = pos[ga];
            const glm::vec3 pb = pos[gb];
            const glm::vec3 mid = (pa + pb) * 0.5f;

            LineInst first{};
            first.aPos = glm::vec4(pa, 0.0f);
            first.bPos = glm::vec4(mid, 0.0f);
            first.color = Representer::colorForAtom(ctx, ga);
            out.append(Cmd::LineInst, &first, sizeof(first));

            LineInst second{};
            second.aPos = glm::vec4(mid, 0.0f);
            second.bPos = glm::vec4(pb, 0.0f);
            second.color = Representer::colorForAtom(ctx, gb);
            out.append(Cmd::LineInst, &second, sizeof(second));
        }
    }

} // namespace moleculens::engine
