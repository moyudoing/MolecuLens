// MolecuLens · shaders/triangle.frag —— M0 清屏测试着色器

#version 450

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
