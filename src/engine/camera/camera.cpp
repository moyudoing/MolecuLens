// =============================================================================
// MolecuLens · engine · camera/camera.cpp —— 轨道相机实现
// 规格来源：主方案 §5.5
// =============================================================================

#include "engine/camera/camera.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace moleculens::engine {

glm::vec3 OrbitCamera::position() const {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    // 相机在 target 的 -Y+ 方向（pitch>0 从上往下看）
    return target + distance * glm::vec3(cp * sy, sp, cp * cy);
}

glm::mat4 OrbitCamera::viewMatrix() const {
    glm::mat4 v = glm::lookAt(position(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    // 绕屏幕法线（视图空间 +Z）滚转：view' = Rz(roll) * view（先 lookAt 再滚转
    // 视图空间坐标 → 图像绕屏幕中心旋转；正 roll = 逆时针，见 rollView 注释）
    if (roll != 0.0f) {
        v = glm::rotate(glm::mat4(1.0f), roll, glm::vec3(0.0f, 0.0f, 1.0f)) * v;
    }
    return v;
}

glm::mat4 OrbitCamera::projectionMatrix(float aspect) const {
    if (ortho) {
        const float halfH = distance * orthoScale * 0.05f;
        const float halfW = halfH * aspect;
        return glm::ortho(-halfW, halfW, -halfH, halfH, nearClip, farClip);
    }
    return glm::perspective(glm::radians(fov), aspect, nearClip, farClip);
}

glm::mat4 OrbitCamera::viewProjMatrix(float aspect) const {
    return projectionMatrix(aspect) * viewMatrix();
}

void OrbitCamera::orbit(float dx, float dy) {
    yaw += dx;
    pitch = std::clamp(pitch + dy, -1.55f, 1.55f);
}

void OrbitCamera::dolly(float factor) {
    distance = std::clamp(distance * factor, nearClip * 10.0f, farClip * 0.5f);
}

void OrbitCamera::pan(const glm::vec2& ndcDelta) {
    // 屏幕空间平移：按当前距离换算世界单位
    const float worldPerPx = distance * std::tan(glm::radians(fov) * 0.5f) * 2.0f;
    const glm::mat4 view = viewMatrix();
    const glm::vec3 right = glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0]));
    const glm::vec3 up = glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1]));
    target -= (right * ndcDelta.x + up * ndcDelta.y) * worldPerPx;
}

void OrbitCamera::rollView(float delta) {
    // 累计滚转角（viewMatrix 以 Rz(roll) 后乘应用）。
    // 交互层传入 delta：右键拖拽，正值 = 视图逆时针（见 MolScene::handleMouse）。
    roll += delta;
}

void OrbitCamera::setFov(float degrees) {
    fov = std::clamp(degrees, 10.0f, 120.0f);
}

void OrbitCamera::centerOn(const glm::vec3& center, float radius) {
    target = center;
    const float r = std::max(radius, 1e-3f);
    // distance = radius / tan(fov/2) * 1.2（主方案 §5.5）
    distance = r / std::tan(glm::radians(fov) * 0.5f) * 1.2f;
    nearClip = r * 1e-3f;
    farClip = r * 10.0f;
}

} // namespace moleculens::engine
