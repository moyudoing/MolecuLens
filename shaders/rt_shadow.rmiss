// =============================================================================
// MolecuLens · shaders/rt_shadow.rmiss —— 阴影射线未命中（= 完全受光）
// 规格来源：任务 E（GPU 光追模块）
// 说明：阴影射线用 SkipClosestHitShader + TerminateOnFirstHit，命中时不会写
//       payload（保持 raygen 置的 0 = 被遮挡）；只有未命中走到这里置 1。
// =============================================================================
#version 460
#extension GL_GOOGLE_include_directive : require
// 阴影 miss 只写回阴影载荷（入向）；主载荷这里用不到，保持默认出向声明即可
//（同一模块不能有两个 IncomingRayPayloadKHR 变量：VUID-StandaloneSpirv-04700）。
#define MOLECULENS_RT_SHADOW_MISS 1
#include "rt_common.glsl"

void main() {
    gShadow.lit = 1.0;
}
