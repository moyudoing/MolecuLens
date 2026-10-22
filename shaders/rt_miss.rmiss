// =============================================================================
// MolecuLens · shaders/rt_miss.rmiss —— 主光线未命中（背景）
// 规格来源：任务 E（GPU 光追模块）
// 说明：只清 payload（颜色由 raygen 填背景色，与 CPU traceColor 一致）。
// =============================================================================
#version 460
#extension GL_GOOGLE_include_directive : require
#define MOLECULENS_RT_INCOMING_PAYLOAD 1   // miss 写入的是入向载荷
#include "rt_common.glsl"

void main() {
    gPayload.hit = 0;
    gPayload.color = vec3(0.0);
    gPayload.alpha = 1.0;
    gPayload.t = -1.0;
}
