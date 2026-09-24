// =============================================================================
// MolecuLens · shaders/point.frag —— 点模式片元（无光照，纯色点）
// =============================================================================

#version 450

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
