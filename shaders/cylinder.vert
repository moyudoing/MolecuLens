// =============================================================================
// MolecuLens · shaders/cylinder.vert —— 实例化键圆柱（球棍 / 棍状）
// 规格来源：主方案 §5.7.2/§5.7.3（computeEndpoints 后端点即圆柱轴）
// 实例属性布局对应 C++ 载荷 CylinderInst（64B，主方案 §5.3/附录 F）
// 局部几何：单位圆柱（半径 1，y ∈ [-1,1]），实例变换：缩放(radius, len/2, radius) + 对齐
// =============================================================================

#version 450

layout(location = 0) in vec3 aLocalPos;      // 共享单位圆柱顶点

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
    vec4 fog;
    vec4 sceneProps;
} ubo;

// 实例数据（binding 1）：CylinderInst 64B
layout(location = 1) in vec4  instA;         // 端点 A (xyz, 保留)
layout(location = 2) in vec4  instB;         // 端点 B (xyz, 保留)
layout(location = 3) in vec4  instRadius;    // x = 半径, yzw 保留
layout(location = 4) in vec4  instColor;     // RGBA
layout(location = 5) in uvec2 instPickId;    // x = 拾取 ID, y = 0

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vPos;
layout(location = 3) out vec4 vPickId;
layout(location = 4) out float vDist;

void main() {
    vec3 a = instA.xyz;
    vec3 b = instB.xyz;
    float r = instRadius.x;

    vec3 axis = b - a;
    float halfLen = length(axis) * 0.5;
    vec3 dir = halfLen > 1e-6 ? axis / (2.0 * halfLen) : vec3(0.0, 1.0, 0.0);

    // 正交基（避免 Frenet 类病态：选最不正交的轴做上向量）
    vec3 up = abs(dir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, dir));
    vec3 up2 = cross(dir, right);

    vec3 local = aLocalPos;
    // 键圆柱以两端点中点为原点（local.y=-1 → 端点 A，+1 → 端点 B）。
    // 若以 a 为原点（a + dir*(local.y*halfLen)），圆柱将偏移半个键长——
    // 端点错位、化学键与原子不连接（见 moleculens-fixes）。
    vec3 mid = 0.5f * (a + b);
    vec3 worldPos = mid + dir * (local.y * halfLen) + right * (local.x * r) + up2 * (local.z * r);
    vec3 worldN = normalize(right * local.x + up2 * local.z);

    gl_Position = ubo.viewProj * vec4(worldPos, 1.0);
    vColor = instColor;
    vNormal = worldN;
    vPos = worldPos;
    vPickId = vec4(vec2(instPickId), 0.0, 0.0);
    vDist = length(worldPos - ubo.cameraPos.xyz);
}
