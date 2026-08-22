// =============================================================================
// MolecuLens · engine · picking/picking.h —— 拾取
// 规格来源：主方案 §5.6（首选离屏 ID Pass R32_UINT；CPU 射线兜底；
//           pickId → CoordsSoA::atomIds[globalIndex] → (instance, atom)）
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::entity {
class SceneStructure;
}

namespace moleculens::engine {

// 拾取结果：(instance, atom) 定位
struct PickResult {
    bool hit = false;
    uint32_t instanceIndex = 0;
    uint32_t atomIndex = 0;        // 实例内原子下标
    uint32_t globalAtomId = 0;     // CoordsSoA::atomIds 对应值
    glm::vec3 position{0.0f};
};

// CPU 兜底拾取：鼠标射线与实例包围球求交（命中最近者，主方案 §5.6）
class PickRay {
public:
    PickRay() = default;
    PickRay(const glm::vec3& origin, const glm::vec3& dir)
        : origin_(origin), dir_(glm::normalize(dir)) {}

    const glm::vec3& origin() const { return origin_; }
    const glm::vec3& dir() const { return dir_; }

    // 与球 (center, radius) 求交；命中返回最近距离
    bool intersectSphere(const glm::vec3& center, float radius, float* outDist) const;

    // 全场景求交（实例 × 原子，vdW 半径 × 球棍缩放 0.25~0.6 语义由调用方定）
    static PickResult pickScene(const entity::SceneStructure& scene,
                                const glm::vec3& rayOrigin,
                                const glm::vec3& rayDir);

private:
    glm::vec3 origin_{0.0f};
    glm::vec3 dir_{0.0f, 0.0f, -1.0f};
};

// ID Pass 结果回查（GPU 读回 pickId → (instance, atom)，主方案 §5.6）
PickResult resolvePickId(const entity::SceneStructure& scene, uint32_t pickId);

} // namespace moleculens::engine
