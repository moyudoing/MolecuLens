// =============================================================================
// MolecuLens · app · data/data_manager.h —— 数据门面（面板只读访问）
// 规格来源：主方案 §7.1（面板不直接碰渲染底层；DataManager 对 Core/控制器
//           的只读门面：分子列表/当前分子/原子键/统计）
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace moleculens::entity {
class SceneStructure;
}

namespace moleculens::app {

class DataManager {
public:
    void setScene(const entity::SceneStructure* scene) { scene_ = scene; }

    // 分子列表
    size_t moleculeCount() const;
    std::string moleculeName(size_t index) const;
    bool moleculeVisible(size_t index) const;
    size_t atomCount(size_t index) const;
    size_t bondCount(size_t index) const;
    size_t frameCount(size_t index) const;
    // 轨迹来源文件名（无轨迹返回空串）；帧间隔 ps（无轨迹返回 0）
    std::string moleculeTrajectorySource(size_t index) const;
    float moleculeTrajectoryDt(size_t index) const;
    // 实例指向的原子结构下标（同一结构的多个实例共享一份原子/键数据；
    // 分子列表按它聚合，避免一个水盒刷出几千行同名条目）
    uint32_t moleculeStructureIndex(size_t index) const;

    // 体系（组）列表：以体系为单位管理
    size_t systemCount() const;
    std::string systemName(size_t systemIndex) const;
    bool systemVisible(size_t systemIndex) const;
    uint32_t systemId(size_t systemIndex) const;                 // → SceneStructure 方法
    uint32_t systemIdOfInstance(size_t instanceIndex) const;     // UINT32_MAX = 未归组
    size_t systemMemberCount(size_t systemIndex) const;
    // 体系内第 pos 个成员 → 全局实例下标（再经 moleculeName/atomCount 等查询）
    uint32_t systemMemberInstance(size_t systemIndex, size_t posInSystem) const;

private:
    const entity::SceneStructure* scene_ = nullptr;
};

} // namespace moleculens::app
