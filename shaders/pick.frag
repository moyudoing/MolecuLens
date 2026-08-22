// =============================================================================
// MolecuLens · shaders/pick.frag —— 拾取 ID Pass 片元着色器
// 规格来源：主方案 §5.6（ID 写入 R32_UINT 离屏纹理；背景 = 0 = 未命中）
// =============================================================================

#version 450

layout(location = 0) in flat uint vPickId;
layout(location = 0) out uint outId;

void main() {
    outId = vPickId;
}
