// =============================================================================
// MolecuLens · shaders/pick_cyl.vert —— 拾取 ID Pass（圆柱实例化）
// 实例布局 = CylinderInst（64B）：a/b/radius/color/pickId
// =============================================================================

#version 450

layout(location = 0) in vec3 aLocalPos;

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
} ubo;

layout(location = 1) in vec4  instA;
layout(location = 2) in vec4  instB;
layout(location = 3) in vec4  instRadius;
layout(location = 4) in vec4  instColor;
layout(location = 5) in uvec2 instPickId;

layout(location = 0) out flat uint vPickId;

void main() {
    vec3 a = instA.xyz;
    vec3 b = instB.xyz;
    float r = instRadius.x;
    vec3 axis = b - a;
    float halfLen = length(axis) * 0.5;
    vec3 dir = halfLen > 1e-6 ? axis / (2.0 * halfLen) : vec3(0.0, 1.0, 0.0);
    vec3 up = abs(dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, dir));
    vec3 up2 = cross(dir, right);
    vec3 worldPos = a + dir * (aLocalPos.y * halfLen)
                     + right * (aLocalPos.x * r) + up2 * (aLocalPos.z * r);
    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vPickId = instPickId.x;
}
