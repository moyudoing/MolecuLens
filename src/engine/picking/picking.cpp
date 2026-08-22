// =============================================================================
// MolecuLens · engine · picking/picking.cpp —— 拾取实现
// 规格来源：主方案 §5.6
// =============================================================================

#include "engine/picking/picking.h"

#include <algorithm>
#include <limits>

#include "entity/element/element.h"
#include "entity/scene/scene.h"

namespace moleculens::engine {

bool PickRay::intersectSphere(const glm::vec3& center, float radius,
                              float* outDist) const {
    const glm::vec3 oc = origin_ - center;
    const float b = glm::dot(oc, dir_);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f) {
        return false;
    }
    const float sq = std::sqrt(disc);
    const float t0 = -b - sq;
    const float t1 = -b + sq;
    const float t = t0 >= 0.0f ? t0 : t1;
    if (t < 0.0f) {
        return false;
    }
    if (outDist) {
        *outDist = t;
    }
    return true;
}

PickResult PickRay::pickScene(const entity::SceneStructure& scene,
                              const glm::vec3& rayOrigin,
                              const glm::vec3& rayDir) {
    PickResult best;
    float bestDist = std::numeric_limits<float>::max();
    const auto& coords = scene.coordsSoA();
    for (uint32_t instIdx = 0; instIdx < scene.instances().size(); ++instIdx) {
        const auto& inst = scene.instances()[instIdx];
        if (!inst.visible) {
            continue;
        }
        const size_t n = scene.structures()[inst.structureIndex].elements.size();
        const uint32_t base = scene.globalAtomBase(instIdx);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t g = base + static_cast<uint32_t>(i);
            if (g >= coords.positions.size()) {
                break;
            }
            // 拾取半径：球棍视觉半径（0.25×vdW clamp 0.2~0.6）
            const float vdw = elementInfo(
                scene.structures()[inst.structureIndex].elements[i]).vdwRadius;
            const float r = std::clamp(0.25f * vdw, 0.2f, 0.6f);
            float dist = 0.0f;
            if (PickRay(rayOrigin, rayDir).intersectSphere(coords.positions[g], r, &dist) &&
                dist < bestDist) {
                bestDist = dist;
                best.hit = true;
                best.instanceIndex = instIdx;
                best.atomIndex = static_cast<uint32_t>(i);
                best.globalAtomId = coords.atomIds[g];
                best.position = coords.positions[g];
            }
        }
    }
    return best;
}

PickResult resolvePickId(const entity::SceneStructure& scene, uint32_t pickId) {
    PickResult result;
    if (pickId == 0) {
        return result;   // 背景 = 未命中
    }
    // pickId = 全局原子 ID（CoordsSoA::atomIds 值）
    const auto& ids = scene.coordsSoA().atomIds;
    for (size_t g = 0; g < ids.size(); ++g) {
        if (ids[g] == pickId) {
            result.hit = true;
            result.globalAtomId = pickId;
            if (g < scene.coordsSoA().positions.size()) {
                result.position = scene.coordsSoA().positions[g];
            }
            // 定位 (instance, atom)
            uint32_t base = 0;
            for (uint32_t instIdx = 0; instIdx < scene.instances().size(); ++instIdx) {
                const size_t n =
                    scene.structures()[scene.instances()[instIdx].structureIndex]
                        .elements.size();
                if (g < base + n) {
                    result.instanceIndex = instIdx;
                    result.atomIndex = static_cast<uint32_t>(g - base);
                    break;
                }
                base += static_cast<uint32_t>(n);
            }
            break;
        }
    }
    return result;
}

} // namespace moleculens::engine
