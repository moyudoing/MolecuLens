// =============================================================================
// MolecuLens · engine · raytrace/ray_tracer.cpp —— CPU 光线追踪器实现
// 规格来源：一.7 自研光线追踪；主方案 §6.6（Blinn-Phong 光照）
// 算法：逐像素主光线 → 球/胶囊（圆柱面+球形帽）解析求交 → 最近命中 →
//       Blinn-Phong + 软阴影（光源方向锥内确定性抖动采样）+ 球体反射（1 次）。
//       按行多线程并行；确定性伪随机（无共享状态，线程安全）。
// =============================================================================

#include "engine/raytrace/ray_tracer.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace moleculens::engine {

namespace {

constexpr float kEps = 1e-4f;

// 材质（与 GPU MaterialBlock 同义；命中时随图元带出，供着色使用）
struct RtMaterial {
    glm::vec3 color{0.0f};      // diffuse × 图元色（已烘焙）
    glm::vec3 ambient{0.60f};
    glm::vec3 specular{0.30f};
    float shininess = 32.0f;
    float roughness = 0.40f;
    float metallic = 0.0f;
    float opacity = 1.0f;      // <1 = 半透明（继续追踪后方并混合）
};

struct RtHit {
    float t = 1e30f;
    glm::vec3 pos{0.0f};
    glm::vec3 n{0.0f, 1.0f, 0.0f};
    RtMaterial mat;
    bool hit = false;
};

// 确定性伪随机（线程安全，无共享状态）：[0,1)
inline float hashUnit(uint32_t a, uint32_t b) {
    uint32_t x = a * 374761393u + b * 668265263u;
    x = (x ^ (x >> 13)) * 1274126177u;
    x ^= x >> 16;
    return (x & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// 光线-球求交（返回最近正 t）
bool hitSphere(const glm::vec3& o, const glm::vec3& d, const glm::vec3& c, float r,
               float* outT, glm::vec3* outN) {
    const glm::vec3 oc = o - c;
    const float b = glm::dot(oc, d);
    const float cc = glm::dot(oc, oc) - r * r;
    const float disc = b * b - cc;
    if (disc < 0.0f) {
        return false;
    }
    const float sq = std::sqrt(disc);
    float t = -b - sq;
    if (t < kEps) {
        t = -b + sq;
    }
    if (t < kEps) {
        return false;
    }
    *outT = t;
    *outN = glm::normalize((o + t * d) - c);
    return true;
}

// 光线-胶囊（两端球形帽 + 圆柱面）求交
bool hitCapsule(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a,
                const glm::vec3& b, float r, float* outT, glm::vec3* outN) {
    const glm::vec3 axis = b - a;
    const float len = glm::length(axis);
    float bestT = 1e30f;
    glm::vec3 bestN(0.0f, 1.0f, 0.0f);
    const auto consider = [&](float t, const glm::vec3& n) {
        if (t > kEps && t < bestT) {
            bestT = t;
            bestN = n;
        }
    };
    if (len > 1e-8f) {
        const glm::vec3 au = axis / len;
        // 圆柱面：把射线分解为沿轴 / 垂直轴，垂直距离 = r 求解
        const glm::vec3 e = o - a;
        const glm::vec3 pe = e - au * glm::dot(e, au);
        const glm::vec3 pd = d - au * glm::dot(d, au);
        const float A = glm::dot(pd, pd);
        if (A > 1e-8f) {
            const float B = glm::dot(pe, pd);
            const float C = glm::dot(pe, pe) - r * r;
            const float disc = B * B - A * C;
            if (disc >= 0.0f) {
                const float sq = std::sqrt(disc);
                const float t0 = (-B - sq) / A;
                const float t1 = (-B + sq) / A;
                const float uu0 = glm::dot(e + t0 * d, au);
                if (t0 > kEps && uu0 >= 0.0f && uu0 <= len) {
                    const glm::vec3 p = o + t0 * d;
                    consider(t0, glm::normalize(p - (a + au * uu0)));
                }
                const float uu1 = glm::dot(e + t1 * d, au);
                if (t1 > kEps && uu1 >= 0.0f && uu1 <= len) {
                    const glm::vec3 p = o + t1 * d;
                    consider(t1, glm::normalize(p - (a + au * uu1)));
                }
            }
        }
    } else {
        // 退化（a==b）：当球处理
        float t;
        glm::vec3 n;
        if (hitSphere(o, d, a, r, &t, &n)) {
            consider(t, n);
        }
    }
    // 两端球形帽
    {
        float t;
        glm::vec3 n;
        if (hitSphere(o, d, a, r, &t, &n)) {
            consider(t, n);
        }
        if (hitSphere(o, d, b, r, &t, &n)) {
            consider(t, n);
        }
    }
    if (bestT >= 1e29f) {
        return false;
    }
    *outT = bestT;
    *outN = bestN;
    return true;
}

// —— BVH 加速结构（P1 光追原型）——
// 场景图元是球与胶囊（圆柱+两端球形帽）。逐个线性求交在 6452 球 + 4436 键上
// 每根光线要测 ~1.1 万个图元，阴影光线再乘采样数；这里按质心中位数递归二分
// 建 BVH，主光线取最近命中、阴影光线做提前退出的遮挡测试。
// 说明：BVH 只改变"测试哪些图元"，命中判定仍是原来的 hitSphere/hitCapsule，
//       因此结果与线性版本逐像素一致（最近命中唯一）。
struct BvhPrim {
    glm::vec3 bmin{0.0f};
    glm::vec3 bmax{0.0f};
    glm::vec3 centroid{0.0f};
    uint32_t kind = 0;    // 0 = sphere, 1 = cylinder
    uint32_t index = 0;   // 索引到 scene.spheres / scene.cylinders
};

struct Bvh {
    struct Node {
        glm::vec3 bmin{0.0f};
        glm::vec3 bmax{0.0f};
        int32_t left = -1;    // 内部节点：左子下标
        int32_t right = -1;   // 内部节点：右子下标
        int32_t first = 0;    // 叶节点：prims 起始下标
        int32_t count = 0;    // 叶节点：图元数
        bool leaf() const { return left < 0; }
    };
    std::vector<Node> nodes;
    std::vector<BvhPrim> prims;

    void build(const RtScene& scene) {
        prims.clear();
        nodes.clear();
        prims.reserve(scene.spheres.size() + scene.cylinders.size());
        for (size_t i = 0; i < scene.spheres.size(); ++i) {
            const auto& s = scene.spheres[i];
            BvhPrim p;
            p.bmin = s.center - glm::vec3(s.radius);
            p.bmax = s.center + glm::vec3(s.radius);
            p.centroid = s.center;
            p.kind = 0;
            p.index = static_cast<uint32_t>(i);
            prims.push_back(p);
        }
        for (size_t i = 0; i < scene.cylinders.size(); ++i) {
            const auto& c = scene.cylinders[i];
            BvhPrim p;
            // 胶囊的 AABB：两端点包围盒外扩半径（略保守，但绝不会漏）
            const glm::vec3 lo = glm::min(c.a, c.b) - glm::vec3(c.radius);
            const glm::vec3 hi = glm::max(c.a, c.b) + glm::vec3(c.radius);
            p.bmin = lo;
            p.bmax = hi;
            p.centroid = 0.5f * (c.a + c.b);
            p.kind = 1;
            p.index = static_cast<uint32_t>(i);
            prims.push_back(p);
        }
        for (size_t i = 0; i < scene.triangles.size(); ++i) {
            const auto& tr = scene.triangles[i];
            BvhPrim p;
            p.bmin = glm::min(glm::min(tr.v0, tr.v1), tr.v2);
            p.bmax = glm::max(glm::max(tr.v0, tr.v1), tr.v2);
            p.centroid = (tr.v0 + tr.v1 + tr.v2) / 3.0f;
            p.kind = 2;
            p.index = static_cast<uint32_t>(i);
            prims.push_back(p);
        }
        if (prims.empty()) {
            nodes.clear();
            return;
        }
        // 预留满二叉最坏节点数（2n-1），并行构建时用原子游标领节点，避免 vector
        // 扩容让其他线程的引用失效。
        nodes.assign(prims.size() * 2 + 1, Node{});
        std::atomic<int32_t> cursor{0};
        std::atomic<int> budget{threadBudget()};
        buildRange(0, static_cast<int32_t>(prims.size()), 0, cursor, budget);
        nodes.resize(static_cast<size_t>(cursor.load()));
    }

    // 并行线程预算：默认 min(8, 硬件线程/2)；MOLECULENS_RT_THREADS 可覆盖
    static int threadBudget() {
        static const int kBudget = [] {
            if (const char* env = std::getenv("MOLECULENS_RT_THREADS")) {
                const int v = std::atoi(env);
                if (v > 0) {
                    return std::clamp(v, 1, 64);
                }
            }
            const auto hw = static_cast<int>(std::thread::hardware_concurrency());
            return std::clamp(hw / 2, 1, 8);
        }();
        return kBudget;
    }

    // 返回节点下标。并行策略：区间够大且深度浅时，左子交给新线程、右子本线程算；
    // 两者只写 nodes 的不同下标、只切分 prims 的不相交区间 → 无需加锁。
    int32_t buildRange(int32_t first, int32_t count, int depth, std::atomic<int32_t>& cursor,
                       std::atomic<int>& budget) {
        constexpr int32_t kLeafSize = 4;
        constexpr int32_t kSpawnMinPrims = 8192;   // 小任务开线程不值得
        constexpr int kMaxSpawnDepth = 6;
        const int32_t nodeIdx = cursor.fetch_add(1, std::memory_order_relaxed);
        glm::vec3 bmin(1e30f), bmax(-1e30f);
        glm::vec3 cmin(1e30f), cmax(-1e30f);
        for (int32_t i = 0; i < count; ++i) {
            const BvhPrim& p = prims[static_cast<size_t>(first + i)];
            bmin = glm::min(bmin, p.bmin);
            bmax = glm::max(bmax, p.bmax);
            cmin = glm::min(cmin, p.centroid);
            cmax = glm::max(cmax, p.centroid);
        }
        Node& node = nodes[static_cast<size_t>(nodeIdx)];
        node.bmin = bmin;
        node.bmax = bmax;
        if (count <= kLeafSize || depth >= 40) {
            node.first = first;
            node.count = count;
            return nodeIdx;
        }
        // 最长轴 + 质心中位数二分
        const glm::vec3 ext = cmax - cmin;
        int axis = 0;
        if (ext.y > ext[axis]) {
            axis = 1;
        }
        if (ext.z > ext[axis]) {
            axis = 2;
        }
        if (ext[axis] < 1e-6f) {
            node.first = first;
            node.count = count;
            return nodeIdx;
        }
        const int32_t mid = first + count / 2;
        std::nth_element(prims.begin() + first, prims.begin() + mid, prims.begin() + first + count,
                         [axis](const BvhPrim& a, const BvhPrim& b) {
                             return a.centroid[axis] < b.centroid[axis];
                         });
        const int32_t leftCount = mid - first;
        int32_t left = -1;
        int32_t right = -1;
        bool spawned = false;
        if (count >= kSpawnMinPrims && depth < kMaxSpawnDepth) {
            int expected = budget.load(std::memory_order_relaxed);
            while (expected > 0 && !budget.compare_exchange_weak(expected, expected - 1)) {
            }
            if (expected > 0) {
                std::thread worker([&] {
                    left = buildRange(first, leftCount, depth + 1, cursor, budget);
                });
                right = buildRange(mid, count - leftCount, depth + 1, cursor, budget);
                worker.join();
                spawned = true;
            }
        }
        if (!spawned) {
            left = buildRange(first, leftCount, depth + 1, cursor, budget);
            right = buildRange(mid, count - leftCount, depth + 1, cursor, budget);
        }
        node.right = right;
        node.left = left;
        node.count = 0;
        return nodeIdx;
    }
};

// 光线—AABB（slab 法）；返回是否有交（tmin 未用，只做剔除）
inline bool hitAabb(const glm::vec3& bmin, const glm::vec3& bmax, const glm::vec3& o,
                    const glm::vec3& invD, float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        float tn = (bmin[a] - o[a]) * invD[a];
        float tf = (bmax[a] - o[a]) * invD[a];
        if (tn > tf) {
            std::swap(tn, tf);
        }
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}

// 当前追踪使用的 BVH（rayTrace() 入口构建，追踪期间只读 → 多线程安全）
const Bvh* g_activeBvh = nullptr;

// 光线—三角形（Möller–Trumbore）；返回 t 与重心坐标 (u, v)
inline bool hitTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& v0,
                        const glm::vec3& v1, const glm::vec3& v2, float* tOut,
                        float* uOut, float* vOut) {
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 pvec = glm::cross(d, e2);
    const float det = glm::dot(e1, pvec);
    if (std::fabs(det) < 1e-9f) {
        return false;   // 平行
    }
    const float invDet = 1.0f / det;
    const glm::vec3 tvec = o - v0;
    const float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::vec3 qvec = glm::cross(tvec, e1);
    const float v = glm::dot(d, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = glm::dot(e2, qvec) * invDet;
    if (t <= kEps) {
        return false;
    }
    *tOut = t;
    *uOut = u;
    *vOut = v;
    return true;
}

// 三角形重心插值法线；退化时回退到几何法线
inline glm::vec3 triangleNormal(const RtTriangle& tri, float u, float v) {
    const float w = 1.0f - u - v;
    glm::vec3 n = w * tri.n0 + u * tri.n1 + v * tri.n2;
    const float len2 = glm::dot(n, n);
    if (len2 < 1e-12f) {
        n = glm::cross(tri.v1 - tri.v0, tri.v2 - tri.v0);
    }
    const float len = std::sqrt(glm::dot(n, n));
    return len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

// 最近命中（线性遍历：无 BVH 时的回退路径）
void traceLinear(const RtScene& scene, const glm::vec3& o, const glm::vec3& d, RtHit& hit) {
    for (const auto& s : scene.spheres) {
        float t;
        glm::vec3 n;
        if (hitSphere(o, d, s.center, s.radius, &t, &n) && t < hit.t) {
            hit.t = t;
            hit.n = n;
            hit.pos = o + t * d;
            hit.mat = RtMaterial{s.color, s.ambient, s.specular, s.shininess, s.roughness,
                                 s.metallic, s.opacity};
            hit.hit = true;
        }
    }
    for (const auto& c : scene.cylinders) {
        float t;
        glm::vec3 n;
        if (hitCapsule(o, d, c.a, c.b, c.radius, &t, &n) && t < hit.t) {
            hit.t = t;
            hit.n = n;
            hit.pos = o + t * d;
            hit.mat = RtMaterial{c.color, c.ambient, c.specular, c.shininess, c.roughness,
                                 c.metallic, c.opacity};
            hit.hit = true;
        }
    }
    for (const auto& tri : scene.triangles) {
        float t, u, v;
        if (hitTriangle(o, d, tri.v0, tri.v1, tri.v2, &t, &u, &v) && t < hit.t) {
            hit.t = t;
            hit.n = triangleNormal(tri, u, v);
            hit.pos = o + t * d;
            hit.mat = RtMaterial{tri.color, tri.ambient, tri.specular, tri.shininess,
                                 tri.roughness, tri.metallic, tri.opacity};
            hit.hit = true;
        }
    }
}

// 最近命中（BVH 遍历：AABB 剔除 + 叶节点内解析求交；命中判定与线性版一致）
void trace(const RtScene& scene, const glm::vec3& o, const glm::vec3& d, RtHit& hit) {
    hit.hit = false;
    hit.t = 1e30f;
    if (g_activeBvh == nullptr || g_activeBvh->nodes.empty()) {
        traceLinear(scene, o, d, hit);
        return;
    }
    const Bvh& bvh = *g_activeBvh;
    const glm::vec3 invD(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    int32_t stack[128];
    int32_t sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Bvh::Node& n = bvh.nodes[static_cast<size_t>(stack[--sp])];
        if (!hitAabb(n.bmin, n.bmax, o, invD, hit.t)) {
            continue;
        }
        if (n.leaf()) {
            for (int32_t i = 0; i < n.count; ++i) {
                const BvhPrim& pr = bvh.prims[static_cast<size_t>(n.first + i)];
                if (pr.kind == 0) {
                    const RtSphere& s = scene.spheres[pr.index];
                    float t;
                    glm::vec3 nn;
                    if (hitSphere(o, d, s.center, s.radius, &t, &nn) && t < hit.t) {
                        hit.t = t;
                        hit.n = nn;
                        hit.pos = o + t * d;
                        hit.mat = RtMaterial{s.color, s.ambient, s.specular, s.shininess,
                                             s.roughness, s.metallic, s.opacity};
                        hit.hit = true;
                    }
                } else if (pr.kind == 1) {
                    const RtCylinder& c = scene.cylinders[pr.index];
                    float t;
                    glm::vec3 nn;
                    if (hitCapsule(o, d, c.a, c.b, c.radius, &t, &nn) && t < hit.t) {
                        hit.t = t;
                        hit.n = nn;
                        hit.pos = o + t * d;
                        hit.mat = RtMaterial{c.color, c.ambient, c.specular, c.shininess,
                                             c.roughness, c.metallic, c.opacity};
                        hit.hit = true;
                    }
                } else {
                    const RtTriangle& tri = scene.triangles[pr.index];
                    float t, u, v;
                    if (hitTriangle(o, d, tri.v0, tri.v1, tri.v2, &t, &u, &v) &&
                        t < hit.t) {
                        hit.t = t;
                        hit.n = triangleNormal(tri, u, v);
                        hit.pos = o + t * d;
                        hit.mat = RtMaterial{tri.color, tri.ambient, tri.specular,
                                             tri.shininess, tri.roughness, tri.metallic,
                                             tri.opacity};
                        hit.hit = true;
                    }
                }
            }
        } else {
            stack[sp++] = n.left;
            stack[sp++] = n.right;
        }
    }
}

// 遮挡测试（阴影光线）：从 origin 沿 dir 方向在 (kEps, maxT) 内是否有任何图元。
// 只判"有无"，不取最近命中——比 trace() 少一半工作；BVH 下命中即返回。
bool occluded(const RtScene& scene, const glm::vec3& origin, const glm::vec3& dir,
              float maxT) {
    float t;
    glm::vec3 n;
    if (g_activeBvh == nullptr || g_activeBvh->nodes.empty()) {
        // 半透明图元不投射实心阴影（与 GPU 侧"透明表面 shadowMask=0"一致）。
        // 否则玻璃壳内部会出现一层与壳同形的黑影。
        for (const auto& s : scene.spheres) {
            if (s.opacity > 0.9f && hitSphere(origin, dir, s.center, s.radius, &t, &n) &&
                t < maxT) {
                return true;
            }
        }
        for (const auto& c : scene.cylinders) {
            if (c.opacity > 0.9f && hitCapsule(origin, dir, c.a, c.b, c.radius, &t, &n) &&
                t < maxT) {
                return true;
            }
        }
        for (const auto& tri : scene.triangles) {
            float u, v;
            if (tri.opacity > 0.9f &&
                hitTriangle(origin, dir, tri.v0, tri.v1, tri.v2, &t, &u, &v) && t < maxT) {
                return true;
            }
        }
        return false;
    }
    const Bvh& bvh = *g_activeBvh;
    const glm::vec3 invD(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);
    int32_t stack[128];
    int32_t sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Bvh::Node& node = bvh.nodes[static_cast<size_t>(stack[--sp])];
        if (!hitAabb(node.bmin, node.bmax, origin, invD, maxT)) {
            continue;
        }
        if (node.leaf()) {
            for (int32_t i = 0; i < node.count; ++i) {
                const BvhPrim& pr = bvh.prims[static_cast<size_t>(node.first + i)];
                if (pr.kind == 0) {
                    const RtSphere& s = scene.spheres[pr.index];
                    float tt;
                    glm::vec3 nn;
                    if (s.opacity > 0.9f &&
                        hitSphere(origin, dir, s.center, s.radius, &tt, &nn) && tt < maxT) {
                        return true;
                    }
                } else if (pr.kind == 1) {
                    const RtCylinder& c = scene.cylinders[pr.index];
                    float tt;
                    glm::vec3 nn;
                    if (c.opacity > 0.9f &&
                        hitCapsule(origin, dir, c.a, c.b, c.radius, &tt, &nn) &&
                        tt < maxT) {
                        return true;
                    }
                } else {
                    const RtTriangle& tri = scene.triangles[pr.index];
                    float tt, u, v;
                    if (tri.opacity > 0.9f &&
                        hitTriangle(origin, dir, tri.v0, tri.v1, tri.v2, &tt, &u, &v) &&
                        tt < maxT) {
                        return true;
                    }
                }
            }
        } else {
            stack[sp++] = node.left;
            stack[sp++] = node.right;
        }
    }
    return false;
}

// 统一光照（与 GPU 侧 material_shading.glsl 的 shadeSurface 同构）：
//   1) 半球环境光：mix(地面色, 天顶色, N.y) × 材质环境色
//   2) 方向光：Lambert 漫反射 + Blinn-Phong 高光，可投软阴影（shadowSamples）
// 说明：gamma 由主交换链 / 离屏 SRGB 附件在写入时完成（见 OffscreenRenderer::
//       createFramebuffer）；本函数输出线性值，末尾 pow(1/2.2) 与之等价。
//   salt：软阴影抖动采样的确定性种子（线程安全，无共享状态）
glm::vec3 shade(const RtScene& scene, const RtLight& light, const RtHit& hit,
                const glm::vec3& viewDir, int depth, uint32_t salt) {
    glm::vec3 N = glm::normalize(hit.n);
    // 法线朝向观察者（内表面/球形帽可能给出背向法线）
    if (glm::dot(N, viewDir) > 0.0f) {
        N = -N;
    }
    const glm::vec3& base = hit.mat.color;
    const float up = std::clamp(N.y * 0.5f + 0.5f, 0.0f, 1.0f);   // 0=朝下 1=朝上
    const glm::vec3 hemi = glm::mix(light.groundColor, light.skyColor, up);
    glm::vec3 col = base * hit.mat.ambient *
                    (light.ambientColor + hemi * light.ambientIntensity) *
                    light.ambientWeight;

    // 2) 方向光（Lambert 漫反射 + Blinn-Phong 高光 + 软阴影），与 GPU 侧同构
    if (light.dirLightEnabled && light.dirLightIntensity > 0.0f) {
        const glm::vec3 L = glm::normalize(light.dirLightDir);
        const glm::vec3 V = -viewDir;
        const float shininess =
            std::max(hit.mat.shininess, 1.0f) * (1.0f - 0.7f * hit.mat.roughness);
        const glm::vec3 specColor = glm::mix(hit.mat.specular, base, hit.mat.metallic);
        const float ndl = std::max(glm::dot(N, L), 0.0f);
        if (ndl > 0.0f) {
            // 软阴影：光源方向锥内确定性抖动采样，取未遮挡比例
            const int samples = std::max(1, light.shadowSamples);
            const glm::vec3 origin = hit.pos + N * kEps;
            float lit = 0.0f;
            for (int s = 0; s < samples; ++s) {
                glm::vec3 ls = L;
                if (samples > 1) {
                    const float j1 =
                        hashUnit(salt * 2654435761u + static_cast<uint32_t>(s),
                                 salt + 17u) - 0.5f;
                    const float j2 =
                        hashUnit(salt + 31u, salt * 40503u + static_cast<uint32_t>(s)) -
                        0.5f;
                    ls = glm::normalize(L + 0.10f * j1 * glm::vec3(1.0f, 0.0f, 0.0f) +
                                        0.10f * j2 * glm::vec3(0.0f, 1.0f, 0.0f));
                }
                if (!occluded(scene, origin, ls, 1e30f)) {
                    lit += 1.0f;
                }
            }
            lit /= static_cast<float>(samples);

            const glm::vec3 H = glm::normalize(L + V);
            const float spec = std::pow(std::max(glm::dot(N, H), 0.0f), shininess);
            col += (base * light.dirLightColor *
                        (ndl * light.dirLightIntensity * light.diffuseWeight) +
                    specColor * light.dirLightColor *
                        (spec * light.dirLightIntensity * light.specularWeight)) *
                   lit;
        }
    }

    // 球体反射（1 次弹射）
    if (depth > 0) {
        const glm::vec3 R = glm::reflect(viewDir, N);
        RtHit r2;
        trace(scene, hit.pos + N * kEps, R, r2);
        if (r2.hit) {
            col += 0.18f * shade(scene, light, r2, R, depth - 1,
                                 salt * 2246822519u + 1u);
        }
    }
    return glm::max(col, glm::vec3(0.0f));
}

// 半透明合成：命中面不透明度 < 1 时，继续沿同一方向追踪其后方，再按
// out = front·α + behind·(1-α) 混合。GPU 光栅路径一直按 MaterialBlock::opacity
// 走混合管线，而 RT 此前完全忽略 opacity —— 用户看到的"光追引擎加载不了透明"
// 就是这里：玻璃壳/半透明表面在 RT 里被当成实心。
// 递归深度上限 kMaxTransparentLayers：多层玻璃壳不会无限递归。
constexpr int kMaxTransparentLayers = 8;

glm::vec3 traceColor(const RtScene& scene, const RtLight& light, const glm::vec3& o,
                     const glm::vec3& d, const glm::vec3& backgroundColor, int depth,
                     uint32_t salt) {
    RtHit hit;
    trace(scene, o, d, hit);
    if (!hit.hit) {
        return backgroundColor;
    }
    const glm::vec3 local = shade(scene, light, hit, d, light.maxBounces, salt);
    const float alpha = std::clamp(hit.mat.opacity, 0.0f, 1.0f);
    if (alpha >= 0.999f || depth <= 0) {
        return local;   // 不透明：与旧行为逐位一致
    }
    // 沿光线方向推进一步，避免再次命中同一表面（自交）。
    const glm::vec3 behindOrigin = hit.pos + d * (kEps * 16.0f);
    const glm::vec3 behind =
        traceColor(scene, light, behindOrigin, d, backgroundColor, depth - 1,
                   salt * 2246822519u + 7u);
    return local * alpha + behind * (1.0f - alpha);
}

} // namespace

bool buildRtScene(const SceneCommandList& list, RtScene& out) {
    out.spheres.clear();
    out.cylinders.clear();
    // 当前材质：P6 起**完整**烘焙（此前只取 diffuse → RT 侧材质预设同样"无效果"）。
    // 默认值 = Representation 的"默认"预设，与 GPU MaterialBlock 同义。
    MaterialBlock cur{};
    cur.ambient = glm::vec4(0.60f, 0.60f, 0.60f, 1.0f);
    cur.diffuse = glm::vec4(0.90f, 0.90f, 0.90f, 1.0f);
    cur.specular = glm::vec4(0.30f, 0.30f, 0.30f, 1.0f);
    cur.shininess = 32.0f;
    cur.metallic = 0.0f;
    cur.roughness = 0.40f;
    cur.opacity = 1.0f;
    CommandIterator it(list);
    // 预扫一遍统计三角形总数，一次性预留：surface 表示按分子切成上千个 TriMesh
    // 命令块，若每块各自 reserve 会在大数组上反复整体搬移（实测 911k 三角形时
    // 总耗时从 0.3 s 涨到 27 s）。这里只走一遍命令头，开销可忽略。
    {
        size_t triTotal = 0;
        CommandIterator pre(list);
        for (bool has = list.first(pre); has; has = list.next(pre)) {
            if (pre.code() != Cmd::TriMesh || pre.payloadSize() < sizeof(TriMeshHeader)) {
                continue;
            }
            TriMeshHeader hdr{};
            std::memcpy(&hdr, pre.payload(), sizeof(TriMeshHeader));
            const size_t need = sizeof(TriMeshHeader) +
                                static_cast<size_t>(hdr.vertexCount) * sizeof(TriMeshVertex) +
                                static_cast<size_t>(hdr.indexCount) * sizeof(uint32_t);
            if (need > pre.payloadSize()) {
                continue;
            }
            triTotal += hdr.indexCount / 3;
        }
        out.triangles.clear();
        out.triangles.reserve(triTotal);
    }
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint8_t* payload = it.payload();
        switch (it.code()) {
            case Cmd::MaterialBlock: {
                if (it.payloadSize() >= sizeof(MaterialBlock)) {
                    std::memcpy(&cur, payload, sizeof(MaterialBlock));
                }
                break;
            }
            case Cmd::SphereInst: {
                const uint32_t n = it.payloadSize() / sizeof(SphereInst);
                for (uint32_t i = 0; i < n; ++i) {
                    SphereInst inst;
                    std::memcpy(&inst, payload + i * sizeof(SphereInst), sizeof(SphereInst));
                    RtSphere s;
                    s.center = glm::vec3(inst.posRadius);
                    s.radius = inst.posRadius.w;
                    s.color = glm::vec3(inst.color) * glm::vec3(cur.diffuse);
                    s.ambient = glm::vec3(cur.ambient);
                    s.specular = glm::vec3(cur.specular);
                    s.shininess = cur.shininess;
                    s.roughness = cur.roughness;
                    s.metallic = cur.metallic;
                    s.opacity = std::clamp(cur.opacity, 0.0f, 1.0f);
                    out.spheres.push_back(s);
                }
                break;
            }
            case Cmd::CylinderInst: {
                const uint32_t n = it.payloadSize() / sizeof(CylinderInst);
                for (uint32_t i = 0; i < n; ++i) {
                    CylinderInst inst;
                    std::memcpy(&inst, payload + i * sizeof(CylinderInst), sizeof(CylinderInst));
                    RtCylinder c;
                    c.a = glm::vec3(inst.aPos);
                    c.b = glm::vec3(inst.bPos);
                    c.radius = inst.radius;
                    c.color = glm::vec3(inst.color) * glm::vec3(cur.diffuse);
                    c.ambient = glm::vec3(cur.ambient);
                    c.specular = glm::vec3(cur.specular);
                    c.shininess = cur.shininess;
                    c.roughness = cur.roughness;
                    c.metallic = cur.metallic;
                    c.opacity = std::clamp(cur.opacity, 0.0f, 1.0f);
                    out.cylinders.push_back(c);
                }
                break;
            }
            case Cmd::TriMesh: {
                if (it.payloadSize() < sizeof(TriMeshHeader)) {
                    break;
                }
                TriMeshHeader hdr{};
                std::memcpy(&hdr, payload, sizeof(TriMeshHeader));
                const uint32_t vertBytes = hdr.vertexCount * sizeof(TriMeshVertex);
                const uint32_t idxBytes = hdr.indexCount * sizeof(uint32_t);
                if (sizeof(TriMeshHeader) + vertBytes + idxBytes > it.payloadSize()) {
                    break;
                }
                const uint8_t* vp = payload + sizeof(TriMeshHeader);
                const uint8_t* ip = vp + vertBytes;
                const auto vertexAt = [&](uint32_t vi) {
                    TriMeshVertex v{};
                    std::memcpy(&v, vp + vi * sizeof(TriMeshVertex), sizeof(TriMeshVertex));
                    return v;
                };
                // 大网格（QuickSurf / surface 动辄数十万三角形）的逐三角形解包是
                // 光追的主要前置开销，按三角形区间切块并行；各块结果按序拼接，
                // 保证图元顺序与串行一致（BVH 输出与像素结果都不变）。
                const uint32_t triCount = hdr.indexCount / 3;
                if (triCount == 0) {
                    break;
                }
                const uint32_t chunkTarget = 32768;   // 每块大约多少三角形
                const uint32_t maxChunks =
                    std::max<uint32_t>(1, static_cast<uint32_t>(Bvh::threadBudget()));
                const uint32_t chunks =
                    std::clamp<uint32_t>((triCount + chunkTarget - 1) / chunkTarget, 1,
                                         maxChunks);
                auto unpackRange = [&](uint32_t begin, uint32_t end,
                                       std::vector<RtTriangle>& dst) {
                    dst.reserve(end - begin);
                    for (uint32_t t = begin; t < end; ++t) {
                        const uint32_t k = t * 3;
                        uint32_t ia = 0, ib = 0, ic = 0;
                        std::memcpy(&ia, ip + k * sizeof(uint32_t), sizeof(uint32_t));
                        std::memcpy(&ib, ip + (k + 1) * sizeof(uint32_t), sizeof(uint32_t));
                        std::memcpy(&ic, ip + (k + 2) * sizeof(uint32_t), sizeof(uint32_t));
                        if (ia >= hdr.vertexCount || ib >= hdr.vertexCount ||
                            ic >= hdr.vertexCount) {
                            continue;
                        }
                        const TriMeshVertex va = vertexAt(ia);
                        const TriMeshVertex vb = vertexAt(ib);
                        const TriMeshVertex vc = vertexAt(ic);
                        RtTriangle tri;
                        tri.v0 = glm::vec3(va.pos);
                        tri.v1 = glm::vec3(vb.pos);
                        tri.v2 = glm::vec3(vc.pos);
                        tri.n0 = glm::vec3(va.normal);
                        tri.n1 = glm::vec3(vb.normal);
                        tri.n2 = glm::vec3(vc.normal);
                        // 顶点色已烘焙材质 diffuse（与 GPU 侧 bakeMaterialIntoColor 一致）
                        const glm::vec3 c0 = glm::vec3(va.color) * glm::vec3(cur.diffuse);
                        const glm::vec3 c1 = glm::vec3(vb.color) * glm::vec3(cur.diffuse);
                        const glm::vec3 c2 = glm::vec3(vc.color) * glm::vec3(cur.diffuse);
                        tri.color = (c0 + c1 + c2) / 3.0f;
                        tri.ambient = glm::vec3(cur.ambient);
                        tri.specular = glm::vec3(cur.specular);
                        tri.shininess = cur.shininess;
                        tri.roughness = cur.roughness;
                        tri.metallic = cur.metallic;
                        tri.opacity = std::clamp(cur.opacity, 0.0f, 1.0f);
                        dst.push_back(tri);
                    }
                };
                if (chunks <= 1) {
                    unpackRange(0, triCount, out.triangles);
                } else {
                    std::vector<std::vector<RtTriangle>> parts(chunks);
                    std::vector<std::thread> workers;
                    workers.reserve(chunks - 1);
                    for (uint32_t c = 1; c < chunks; ++c) {
                        const uint32_t begin = static_cast<uint32_t>(
                            static_cast<uint64_t>(triCount) * c / chunks);
                        const uint32_t end = static_cast<uint32_t>(
                            static_cast<uint64_t>(triCount) * (c + 1) / chunks);
                        workers.emplace_back(
                            [&, begin, end, c] { unpackRange(begin, end, parts[c]); });
                    }
                    unpackRange(0, static_cast<uint32_t>(static_cast<uint64_t>(triCount) / chunks),
                                parts[0]);
                    for (auto& w : workers) {
                        w.join();
                    }
                    for (auto& part : parts) {
                        out.triangles.insert(out.triangles.end(), part.begin(), part.end());
                    }
                }
                break;
            }            default:
                break;
        }
    }
    return true;
}

void rtLightFrom(const LightParams& src, RtLight& out, int shadowSamples,
                 int maxBounces) {
    out.skyColor = src.skyColor;
    out.groundColor = src.groundColor;
    out.ambientColor = src.ambientColor;
    out.ambientIntensity = src.ambientIntensity;
    out.dirLightDir = src.dirLightDir;
    out.dirLightColor = src.dirLightColor;
    out.dirLightIntensity = src.dirLightIntensity;
    out.dirLightEnabled = src.dirLightEnabled;
    out.ambientWeight = src.ambientWeight;
    out.diffuseWeight = src.diffuseWeight;
    out.specularWeight = src.specularWeight;
    out.shadowSamples = shadowSamples;
    out.maxBounces = maxBounces;
}

// —— RT 级 LOD：跨帧缓存的加速结构 ——
// RT 预览一帧的开销分两段：建树（91 万三角形 ~119 ms）与投射。相机移动时
// 场景没变，建树结果可原样复用，于是「拖动手感」只看投射那一段。
struct RtAccel {
    Bvh bvh;
};

std::shared_ptr<RtAccel> buildRtAccel(const RtScene& scene) {
    if (scene.spheres.empty() && scene.cylinders.empty() && scene.triangles.empty()) {
        return nullptr;
    }
    auto accel = std::make_shared<RtAccel>();
    accel->bvh.build(scene);
    return accel;
}

bool rayTrace(const RtScene& scene, const RtCamera& cam, const RtLight& light,
              const glm::vec3& backgroundColor, int samplesPerPixel,
              std::vector<uint8_t>& outRgba, const std::shared_ptr<RtAccel>& accel) {
    const uint32_t W = std::max(1u, cam.width);
    const uint32_t H = std::max(1u, cam.height);
    const int spp = std::max(1, samplesPerPixel);
    outRgba.assign(static_cast<size_t>(W) * H * 4, 0);

    const glm::vec3 forward = glm::normalize(cam.target - cam.position);
    glm::vec3 right = glm::normalize(glm::cross(forward, cam.up));
    const glm::vec3 up2 = glm::normalize(glm::cross(right, forward));
    const float tanHalf = std::tan(glm::radians(cam.fovDeg) * 0.5f);
    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const float px = tanHalf * aspect;
    const float py = tanHalf;

    // —— P1 BVH：整场景建一次，追踪期间只读 ——
    // MOLECULENS_RT_NO_BVH=1 时退回线性遍历（用于 A/B 对比与结果校验）
    // accel 命中（同一命令流的复用建树结果）时跳过建树 —— RT 级 LOD 的关键。
    Bvh localBvh;
    if (std::getenv("MOLECULENS_RT_NO_BVH") == nullptr) {
        if (accel && !accel->bvh.prims.empty()) {
            g_activeBvh = &accel->bvh;
        } else {
            localBvh.build(scene);
            g_activeBvh = &localBvh;
        }
    }

    const auto renderRows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                glm::vec3 acc(0.0f);
                for (int s = 0; s < spp; ++s) {
                    const float jx = spp > 1 ? (hashUnit(x * 97u + static_cast<uint32_t>(s), y * 131u) - 0.5f) : 0.0f;
                    const float jy = spp > 1 ? (hashUnit(x * 151u, y * 89u + static_cast<uint32_t>(s)) - 0.5f) : 0.0f;
                    const float u = ((static_cast<float>(x) + 0.5f + jx) / W) * 2.0f - 1.0f;
                    const float v = 1.0f - ((static_cast<float>(y) + 0.5f + jy) / H) * 2.0f;
                    const glm::vec3 dir =
                        glm::normalize(forward + right * (u * px) + up2 * (v * py));
                    // salt：以像素坐标为种，软阴影抖动在同像素多次采样间一致。
                    // traceColor 内部处理半透明合成；不透明像素与旧 shade 路径一致。
                    const uint32_t salt = (x * 73856093u) ^ (y * 19349663u) ^
                                          (static_cast<uint32_t>(s) * 83492791u);
                    acc += traceColor(scene, light, cam.position, dir, backgroundColor,
                                      kMaxTransparentLayers, salt);
                }
                acc /= static_cast<float>(spp);
                // 线性 → sRGB 近似
                constexpr float kGamma = 1.0f / 2.2f;
                uint8_t* p = &outRgba[(static_cast<size_t>(y) * W + x) * 4];
                p[0] = static_cast<uint8_t>(
                    std::clamp(std::pow(acc.r, kGamma) * 255.0f, 0.0f, 255.0f));
                p[1] = static_cast<uint8_t>(
                    std::clamp(std::pow(acc.g, kGamma) * 255.0f, 0.0f, 255.0f));
                p[2] = static_cast<uint8_t>(
                    std::clamp(std::pow(acc.b, kGamma) * 255.0f, 0.0f, 255.0f));
                p[3] = 255;
            }
        }
    };

    // 按行多线程并行
    const unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t bands = std::min<uint32_t>(H, std::max(1u, cores));
    const uint32_t rowsPer = (H + bands - 1) / bands;
    std::vector<std::thread> workers;
    workers.reserve(bands);
    for (uint32_t b = 0; b < bands; ++b) {
        const uint32_t y0 = b * rowsPer;
        const uint32_t y1 = std::min(H, y0 + rowsPer);
        workers.emplace_back([&, y0, y1]() { renderRows(y0, y1); });
    }
    for (auto& w : workers) {
        w.join();
    }
    g_activeBvh = nullptr;
    return true;
}

} // namespace moleculens::engine
