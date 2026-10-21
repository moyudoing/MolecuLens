// =============================================================================
// MolecuLens · engine · serialize/scene_bridge.h —— 场景 ↔ .mlscene 快照桥接
// 规格来源：主方案 §12.1（场景文件）；persist/mlscene 定义文件格式
//
// 为什么桥接在 engine 层（而不是 persist 或 app）：
//   · 不能放 persist：`src/persist` 编译进 `moleculens_entity`，而本映射需要
//     engine 类型（Representation / OrbitCamera / SceneCommandList）。
//     让 persist 依赖 engine 会形成 entity → engine 的反向依赖。
//   · 不放 app：`moleculens_app` 是可执行文件，测试目标无法链接它 ——
//     放在 app 会让这段映射逻辑完全无法被单元测试覆盖。
//   engine 同时可见 entity 与 persist（engine 依赖 entity，persist 属 entity），
//   是唯一既合法又可测的位置。
//
// 字段对应关系（v2）：
//   Structure.atoms      ← MoleculeStructure::{elements, atoms(occupancy/bfactor/残基/链)}
//                          + atomNameIds（经 StringPool 解析为字符串）
//   Structure.bonds      ← MoleculeStructure::bonds
//   System / Instance    ← SceneStructure::systems() / instances()
//   Representation       ← RepresentationController 持有的表示列表
//   场景属性 / 相机       ← SceneProperties / OrbitCamera
//   commandStream        ← SceneCommandList::writeTo 的字节（persist 视作不透明数据）
//   trajectory           ← 链接式：源文件引用（不内嵌帧坐标）；
//                          嵌入式：帧坐标收进 SceneSnapshot::embeddedTrajectories
//                          （同一 shared_ptr<Trajectory> 只收一次）
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "persist/mlscene/mlscene.h"

namespace moleculens::entity {
class SceneStructure;
}
namespace moleculens::engine {
class OrbitCamera;
class SceneCommandList;
struct Representation;


// —— 保存方向：场景 → 快照 ——
struct SnapshotSource {
    const entity::SceneStructure* scene = nullptr;
    const std::vector<engine::Representation>* representations = nullptr;
    const engine::OrbitCamera* camera = nullptr;
    const engine::SceneCommandList* commandStream = nullptr;   // 可空
    // 轨迹源文件（引用式保存）：由调用方（IoController）从打开的文件路径填装
    std::string trajectorySourcePath;
    uint64_t    trajectorySourceSize = 0;
    int64_t     trajectorySourceMtime = 0;
    // true = 把轨迹帧坐标一起写进快照（嵌入式保存）；false = 只存源文件引用。
    // 嵌入式时同一个 shared_ptr<Trajectory> 只收集一次（多实例体系共享一条轨迹）。
    bool embedTrajectories = false;
    // 场景标注（视图叠加层，v2.3）：由 app 层（Workbench → IoController）提供。
    // nullptr = 不写标注（旧行为）；persist 侧是纯数据，engine 只做拷贝。
    const persist::mlscene::Annotations* annotations = nullptr;
};

persist::mlscene::SceneSnapshot makeSnapshot(const SnapshotSource& in);

// —— 加载方向：快照 → 场景 ——
// 所有输出指针可空（为空的项跳过）。scene 非空时：
//   1) 重建链/残基名字池（按 id 升序 addChain/addResidue —— 解析器也是顺序分配，
//      故 id 稠密，重建后下标与原下标一致）
//   2) registerStructure 注册结构（原子名经 StringPool intern）
//   3) addTrajectoryInstance 添加实例并建立体系分组
struct RestoreTarget {
    entity::SceneStructure* scene = nullptr;
    std::vector<engine::Representation>* representations = nullptr;
    engine::OrbitCamera* camera = nullptr;
    engine::SceneCommandList* commandStream = nullptr;   // 可空
};

void applySnapshot(const persist::mlscene::SceneSnapshot& snap, const RestoreTarget& out);

} // namespace moleculens::engine
