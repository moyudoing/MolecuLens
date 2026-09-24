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
#include <cstring>
#include <thread>

namespace moleculens::engine {

namespace {

constexpr float kEps = 1e-4f;

struct RtHit {
    float t = 1e30f;
    glm::vec3 pos{0.0f};
    glm::vec3 n{0.0f, 1.0f, 0.0f};
    glm::vec3 color{0.0f};
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

// 最近命中
void trace(const RtScene& scene, const glm::vec3& o, const glm::vec3& d, RtHit& hit) {
    hit.hit = false;
    hit.t = 1e30f;
    for (const auto& s : scene.spheres) {
        float t;
        glm::vec3 n;
        if (hitSphere(o, d, s.center, s.radius, &t, &n) && t < hit.t) {
            hit.t = t;
            hit.n = n;
            hit.pos = o + t * d;
            hit.color = s.color;
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
            hit.color = c.color;
            hit.hit = true;
        }
    }
}

// 全局光照（无方向光/点光源）：半球环境光 mix(地面色, 天顶色, N.y) + 均匀环境光
glm::vec3 shade(const RtScene& scene, const RtLight& light, const RtHit& hit,
                const glm::vec3& viewDir, int depth) {
    const glm::vec3& N = hit.n;
    const float up = std::clamp(N.y * 0.5f + 0.5f, 0.0f, 1.0f);   // 0=朝下 1=朝上
    const glm::vec3 hemi = glm::mix(light.groundColor, light.skyColor, up);
    glm::vec3 col = hit.color * (light.ambientColor + hemi * light.ambientIntensity);

    // 球体反射（1 次弹射）
    if (depth > 0) {
        const glm::vec3 R = glm::reflect(viewDir, N);
        RtHit r2;
        trace(scene, hit.pos + N * kEps, R, r2);
        if (r2.hit) {
            col += 0.18f * shade(scene, light, r2, R, depth - 1);
        }
    }
    return glm::max(col, glm::vec3(0.0f));
}

} // namespace

bool buildRtScene(const SceneCommandList& list, RtScene& out) {
    out.spheres.clear();
    out.cylinders.clear();
    glm::vec4 matDiffuse(0.9f, 0.9f, 0.9f, 1.0f);   // 当前材质 diffuse（烘焙进图元色）
    CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        const uint8_t* payload = it.payload();
        switch (it.code()) {
            case Cmd::MaterialBlock: {
                if (it.payloadSize() >= sizeof(MaterialBlock)) {
                    MaterialBlock mat;
                    std::memcpy(&mat, payload, sizeof(MaterialBlock));
                    matDiffuse = mat.diffuse;
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
                    s.color = glm::vec3(inst.color) * glm::vec3(matDiffuse);
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
                    c.color = glm::vec3(inst.color) * glm::vec3(matDiffuse);
                    out.cylinders.push_back(c);
                }
                break;
            }
            default:
                break;
        }
    }
    return true;
}

bool rayTrace(const RtScene& scene, const RtCamera& cam, const RtLight& light,
              const glm::vec3& backgroundColor, int samplesPerPixel,
              std::vector<uint8_t>& outRgba) {
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
                    RtHit hit;
                    trace(scene, cam.position, dir, hit);
                    if (hit.hit) {
                        acc += shade(scene, light, hit, dir, light.maxBounces);
                    } else {
                        // 纯背景色（与光栅化清屏色一致；确定性好，便于测试）
                        acc += backgroundColor;
                    }
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
    return true;
}

} // namespace moleculens::engine
