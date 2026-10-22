// =============================================================================
// MolecuLens · engine · camera/camera.h —— 轨道相机（OrbitCamera）
// 规格来源：主方案 §5.5（target/distance/yaw/pitch/fov/ortho；
//           自动取景 distance = radius/tan(fov/2)×1.2，near ≈ radius×1e-3，
//           far = radius×10；透视/正交可切换）
// =============================================================================

#pragma once

#include <algorithm>

#include <glm/glm.hpp>

namespace moleculens::engine {

class OrbitCamera {
public:
    glm::vec3 target{0.0f};
    float distance = 40.0f;
    float yaw = 0.0f;          // 方位角（弧度）
    float pitch = 0.5f;        // 俯仰角（弧度）
    float roll = 0.0f;         // 绕屏幕法线（视图方向）旋转角（弧度）
    float fov = 45.0f;         // 垂直视场（度）
    bool  ortho = false;
    float orthoScale = 1.0f;   // 正交缩放（世界单位/像素比例系数）
    float nearClip = 0.01f;
    float farClip = 10000.0f;
    // 自动取景得到的包围球半径与缩放边界（dolly 用）。
    // 旧实现把最近距离绑在 nearClip*10 上：nearClip 只由场景半径的 1e-3 决定，
    // 放大时相机可以一路钻进分子内部，近裁剪面把球/键切成碎片，表现为“分子翻形”。
    float boundRadius = 40.0f;
    float minDistance = 1.0f;
    float maxDistance = 5000.0f;

    // 相机位置（轨道推导）
    [[nodiscard]] glm::vec3 position() const;

    // 视图矩阵
    [[nodiscard]] glm::mat4 viewMatrix() const;
    // 投影矩阵（aspect = 宽/高）
    [[nodiscard]] glm::mat4 projectionMatrix(float aspect) const;
    [[nodiscard]] glm::mat4 viewProjMatrix(float aspect) const;

    // 屏幕 NDC → 世界空间拾取射线（主方案 §5.6 CPU 兜底）。
    //   ndcX/ndcY ∈ [-1,1]（原点在视口中心、+y 向上），aspect = 宽/高。
    //   outDir 已归一化；outOrigin = 相机位置。
    // 注：正交投影下按 fov 近似（当前拾取仅用于透视视口）。
    void rayForNdc(float ndcX, float ndcY, float aspect,
                   glm::vec3* outOrigin, glm::vec3* outDir) const;

    // 交互（主方案 §7.10：左键旋转/中键缩放/右键滚转/滚轮 dolly）
    void orbit(float dx, float dy);              // 弧度增量
    void dolly(float factor);                    // 距离缩放
    void pan(const glm::vec2& ndcDelta);         // 平移（屏幕空间）
    void rollView(float delta);                  // 绕屏幕法线旋转（正 = 逆时针）
    void setFov(float degrees);

    // 自动取景（主方案 §5.5）：radius = 包围球半径
    void centerOn(const glm::vec3& center, float radius);

    // 视锥参数
    float aspectAt(float w, float h) const { return w / std::max(h, 1.0f); }
};

} // namespace moleculens::engine
