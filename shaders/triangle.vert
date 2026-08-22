// =============================================================================
// MolecuLens · shaders/triangle.vert —— M0 清屏测试着色器
// 规格来源：M0 工程脚手架 §2 shaders/triangle（清屏测试）
// 输出：屏幕空间固定三角形（验证管线/栅格化/颜色输出链路）
// =============================================================================

#version 450

layout(location = 0) out vec3 vColor;

void main() {
    vec2 positions[3] = vec2[](
        vec2(-0.6, -0.6),
        vec2( 0.6, -0.6),
        vec2( 0.0,  0.6)
    );
    vec3 colors[3] = vec3[](
        vec3(1.0, 0.3, 0.3),
        vec3(0.3, 1.0, 0.3),
        vec3(0.3, 0.3, 1.0)
    );
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vColor = colors[gl_VertexIndex];
}
