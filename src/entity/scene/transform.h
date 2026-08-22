// =============================================================================
// MolecuLens · entity · scene/transform.h —— Transform（平移 + 旋转 + 缩放）
// 规格来源：主方案 §4.3 MoleculeInstance / §5.1 SceneNode（GT·S·R·CT）
// 依赖约束：glm（主方案 §3.5 允许协议头依赖 <cstdint>/<glm>；本头为领域类型）
// =============================================================================

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace moleculens::entity {

// 实例位姿：平移 + 旋转（四元数）+ 均匀缩放
struct Transform {
    glm::vec3    translation{0.0f};
    glm::quat    rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float        scale = 1.0f;

    glm::mat4 matrix() const {
        glm::mat4 m = glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
        m[3] = glm::vec4(translation, 1.0f);
        return m;
    }

    glm::vec3 apply(const glm::vec3& local) const {
        return translation + scale * (rotation * local);
    }

    static Transform identity() { return {}; }
};

} // namespace moleculens::entity
