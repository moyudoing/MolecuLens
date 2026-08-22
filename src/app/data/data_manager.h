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
    bool moleculeVisible() const { return true; }

private:
    const entity::SceneStructure* scene_ = nullptr;
};

} // namespace moleculens::app
