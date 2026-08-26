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

            // 构造线段实例数据 LineInst
            LineInst inst{};
            inst.aPos = glm::vec4(pos[ga], 0.0f);
            inst.bPos = glm::vec4(pos[gb], 0.0f);
            inst.color = Representer::colorForAtom(ctx, ga);

            // 往渲染命令列表追加一条LineInst绘制命令
            out.append(Cmd::LineInst, &inst, sizeof(inst));
        }
    }

} // namespace moleculens::engine
