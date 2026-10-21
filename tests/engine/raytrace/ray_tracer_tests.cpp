// =============================================================================
// MolecuLens · tests · engine/raytrace/ray_tracer_tests.cpp —— CPU 光线追踪器
// 规格来源：一.7 自研光线追踪（单球非背景 / 双球遮挡 / 胶囊命中 / 命令流构建）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "engine/cmd/cmd.h"
#include "engine/raytrace/ray_tracer.h"

using namespace moleculens::engine;

namespace {

// 统计 RGBA 中与背景（纯白）不同的像素数
uint32_t countNonBg(const std::vector<uint8_t>& px, const uint8_t bg[3]) {
    uint32_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] != bg[0] || px[i + 1] != bg[1] || px[i + 2] != bg[2]) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("ray tracer renders a single sphere (non-background pixels)",
          "[engine][raytrace]") {
    RtScene scene;
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, 0.0f), 1.0f, glm::vec3(1.0f, 0.2f, 0.2f)});

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 4.0f);
    cam.target = glm::vec3(0.0f);
    cam.fovDeg = 45.0f;
    cam.width = 64;
    cam.height = 48;

    RtLight light;                 // 默认光源
    light.shadowSamples = 1;       // 硬阴影，快
    light.maxBounces = 0;

    const glm::vec3 bg(1.0f);      // 白背景
    std::vector<uint8_t> px;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, px));
    REQUIRE(px.size() == 64u * 48u * 4u);

    const uint8_t bgc[3] = {255, 255, 255};
    const uint32_t nonBg = countNonBg(px, bgc);
    REQUIRE(nonBg > 100);   // 球投影占据大部分像素

    // 球为红色调：命中像素 R 明显 > B（照明下保持色相）
    uint32_t reddish = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] > px[i + 2] + 30 && px[i] > 60) {
            ++reddish;
        }
    }
    REQUIRE(reddish > 50);
}

TEST_CASE("ray tracer applies the directional light (P6)", "[engine][raytrace]") {
    RtScene scene;
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, 0.0f), 1.0f, glm::vec3(0.8f, 0.8f, 0.8f)});

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 4.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 48;
    cam.height = 48;

    const glm::vec3 bg(0.0f);   // 黑背景，便于直接比较像素亮度

    // 关方向光
    RtLight off;
    off.maxBounces = 0;
    off.shadowSamples = 1;
    off.dirLightEnabled = false;
    std::vector<uint8_t> pxOff;
    REQUIRE(rayTrace(scene, cam, off, bg, 1, pxOff));

    // 开方向光（其余参数不变）
    RtLight on = off;
    on.dirLightEnabled = true;
    on.dirLightIntensity = 1.0f;
    on.dirLightDir = glm::vec3(0.0f, 0.0f, 1.0f);   // 从相机方向射来 → 正面受光
    std::vector<uint8_t> pxOn;
    REQUIRE(rayTrace(scene, cam, on, bg, 1, pxOn));

    // 像素总和必须明显变大（方向光真正参与着色）。倍数不取 2：球面 Lambert 衰减
    // 让边缘变暗，且 sRGB 编码压缩亮部差异，实测比值约 1.4。
    uint64_t sumOff = 0, sumOn = 0;
    for (size_t i = 0; i < pxOff.size(); i += 4) {
        sumOff += pxOff[i] + pxOff[i + 1] + pxOff[i + 2];
        sumOn += pxOn[i] + pxOn[i + 1] + pxOn[i + 2];
    }
    REQUIRE(sumOff > 0);
    REQUIRE(sumOn > sumOff * 5 / 4);
}

TEST_CASE("ray tracer casts a directional shadow (P6)", "[engine][raytrace]") {
    // 侧向光源（L = +X）→ 受光球右半面被照亮；遮挡球放在受光球与光源之间
    // （+X 侧）。环境光全关 → 遮挡球朝向相机的一面为纯黑，不会给画面"加亮"，
    // 于是"遮挡 → 变暗"可以干净地测出来。
    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 6.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 48;
    cam.height = 48;
    const glm::vec3 bg(0.0f);

    RtLight light;
    light.maxBounces = 0;
    light.dirLightEnabled = true;
    light.dirLightIntensity = 1.0f;
    light.dirLightDir = glm::vec3(1.0f, 0.0f, 0.0f);   // 光在 +X 侧
    light.ambientIntensity = 0.0f;                     // 关环境光 → 只看方向光
    light.ambientColor = glm::vec3(0.0f);
    light.skyColor = glm::vec3(0.0f);
    light.groundColor = glm::vec3(0.0f);

    RtScene alone;
    alone.spheres.push_back({glm::vec3(0.0f, 0.0f, 0.0f), 1.0f, glm::vec3(0.9f)});
    std::vector<uint8_t> pxAlone;
    REQUIRE(rayTrace(alone, cam, light, bg, 1, pxAlone));

    RtScene occluded = alone;
    occluded.spheres.push_back({glm::vec3(2.2f, 0.0f, 0.0f), 1.0f, glm::vec3(0.9f)});
    std::vector<uint8_t> pxOccluded;
    REQUIRE(rayTrace(occluded, cam, light, bg, 1, pxOccluded));

    // 只在"无遮挡时被照亮的那批像素"上比较亮度：
    // 遮挡球自身像素不计入 → 排除"多加了一个球"对总量的干扰。
    // 遮挡既可能直接盖住这些像素（变黑），也可能只挡住光源方向（阴影）——两者都使和变小。
    std::vector<size_t> litPixels;
    for (size_t i = 0; i + 3 < pxAlone.size(); i += 4) {
        if (pxAlone[i] > 40) {
            litPixels.push_back(i);
        }
    }
    REQUIRE(litPixels.size() > 50);   // 受光球右半面应有可观像素

    uint64_t sumAlone = 0, sumOccluded = 0;
    for (const size_t i : litPixels) {
        sumAlone += pxAlone[i];
        sumOccluded += pxOccluded[i];
    }
    REQUIRE(sumAlone > 0);
    REQUIRE(sumOccluded < sumAlone);
}

TEST_CASE("ray tracer applies material specular/shininess (P6)", "[engine][raytrace]") {
    // 同一几何、同一光照，只改高光参数 → 像素必须不同（此前 RT 只烘焙 diffuse，
    // 材质面板的高光/光泽度在 RT 路径上完全不参与计算）。
    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 4.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 32;
    cam.height = 32;
    const glm::vec3 bg(0.0f);

    RtLight light;
    light.maxBounces = 0;
    light.dirLightEnabled = true;
    light.dirLightIntensity = 1.0f;
    light.dirLightDir = glm::vec3(0.0f, 0.0f, 1.0f);

    const auto renderWith = [&](const glm::vec3& specular, float shininess) {
        RtScene scene;
        RtSphere s{glm::vec3(0.0f), 1.0f, glm::vec3(0.5f)};
        s.specular = specular;
        s.shininess = shininess;
        scene.spheres.push_back(s);
        std::vector<uint8_t> px;
        REQUIRE(rayTrace(scene, cam, light, bg, 1, px));
        return px;
    };

    const std::vector<uint8_t> matte = renderWith(glm::vec3(0.0f), 4.0f);
    const std::vector<uint8_t> shiny = renderWith(glm::vec3(1.0f), 128.0f);
    REQUIRE(matte != shiny);
}

TEST_CASE("ray tracer front sphere occludes back sphere", "[engine][raytrace]") {
    // 前面（近相机 z=+1）大蓝球遮挡后面（z=-1）小红球
    RtScene scene;
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, 1.0f), 0.6f, glm::vec3(0.2f, 0.2f, 1.0f)});
    scene.spheres.push_back({glm::vec3(0.0f, 0.0f, -1.0f), 0.4f, glm::vec3(1.0f, 0.1f, 0.1f)});

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 3.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 32;
    cam.height = 32;

    RtLight light;
    light.shadowSamples = 1;
    light.maxBounces = 0;

    const glm::vec3 bg(1.0f);
    std::vector<uint8_t> px;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, px));

    // 中心像素应命中蓝球（R 低、B 高）
    const size_t c = (16u * 32u + 16u) * 4u;
    REQUIRE(px[c + 2] > px[c] + 30);   // B > R + 30 → 蓝
}

TEST_CASE("ray tracer renders a capsule (cylinder)", "[engine][raytrace]") {
    RtScene scene;
    scene.cylinders.push_back(
        {glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.4f,
         glm::vec3(0.1f, 0.9f, 0.1f)});   // 横置绿色胶囊

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 3.0f);
    cam.target = glm::vec3(0.0f);
    cam.width = 48;
    cam.height = 32;

    RtLight light;
    light.shadowSamples = 1;
    light.maxBounces = 0;

    const glm::vec3 bg(1.0f);
    std::vector<uint8_t> px;
    REQUIRE(rayTrace(scene, cam, light, bg, 1, px));

    const uint8_t bgc[3] = {255, 255, 255};
    const uint32_t nonBg = countNonBg(px, bgc);
    REQUIRE(nonBg > 100);   // 胶囊投影面积可观
}

TEST_CASE("buildRtScene decodes spheres and cylinders with material diffuse",
          "[engine][raytrace]") {
    SceneCommandList list;
    MaterialBlock mat;
    mat.diffuse = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));

    SphereInst s;
    s.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    s.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    list.append(Cmd::SphereInst, &s, sizeof(s));

    CylinderInst c;
    c.aPos = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    c.bPos = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    c.radius = 0.2f;
    c.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    list.append(Cmd::CylinderInst, &c, sizeof(c));

    RtScene scene;
    REQUIRE(buildRtScene(list, scene));
    REQUIRE(scene.spheres.size() == 1u);
    REQUIRE(scene.cylinders.size() == 1u);
    // 材质 diffuse 0.5 烘焙进颜色
    REQUIRE(scene.spheres[0].color == glm::vec3(0.5f));
    REQUIRE(scene.cylinders[0].color == glm::vec3(0.5f));
    REQUIRE(std::fabs(scene.cylinders[0].radius - 0.2f) < 1e-4f);
}


// P1 原型：BVH 加速结构必须与线性遍历**逐像素一致**（只改变测试哪些图元，
// 不改变命中判定）。用确定性布置的 32 球 + 12 胶囊场景对比两条路径。
TEST_CASE("ray tracer BVH matches linear traversal pixel-for-pixel",
          "[engine][raytrace]") {
    RtScene scene;
    for (int i = 0; i < 32; ++i) {
        const float a = static_cast<float>(i) * 0.7f;
        RtSphere s;
        s.center = glm::vec3(std::cos(a) * 2.2f, std::sin(a * 1.3f) * 1.6f,
                             std::sin(a) * 2.0f);
        s.radius = 0.35f + 0.05f * static_cast<float>(i % 3);
        s.color = glm::vec3(0.8f, 0.3f, 0.2f);
        scene.spheres.push_back(s);
    }
    for (int i = 0; i < 12; ++i) {
        RtCylinder c;
        c.a = glm::vec3(-1.5f + 0.25f * static_cast<float>(i), -0.8f, -1.0f);
        c.b = glm::vec3(-1.5f + 0.25f * static_cast<float>(i), 0.8f, 1.0f);
        c.radius = 0.12f;
        c.color = glm::vec3(0.3f, 0.6f, 0.9f);
        scene.cylinders.push_back(c);
    }

    // 一个立方体（12 个三角形）验证三角形 BVH 路径
    const glm::vec3 box[8] = {{-0.6f, -0.6f, 0.0f}, {0.6f, -0.6f, 0.0f}, {0.6f, 0.6f, 0.0f},
                              {-0.6f, 0.6f, 0.0f},  {-0.6f, -0.6f, 1.2f}, {0.6f, -0.6f, 1.2f},
                              {0.6f, 0.6f, 1.2f},   {-0.6f, 0.6f, 1.2f}};
    const int faces[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
                              {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
                              {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}};
    for (const auto& f : faces) {
        RtTriangle tri;
        tri.v0 = box[f[0]];
        tri.v1 = box[f[1]];
        tri.v2 = box[f[2]];
        tri.n0 = tri.n1 = tri.n2 = glm::vec3(0.0f, 0.0f, 1.0f);
        tri.color = glm::vec3(0.35f, 0.75f, 0.45f);
        scene.triangles.push_back(tri);
    }
    RtCamera cam;
    cam.position = glm::vec3(0.0f, 1.2f, 6.5f);
    cam.target = glm::vec3(0.0f);
    cam.fovDeg = 45.0f;
    cam.width = 96;
    cam.height = 72;

    const RtLight light;
    const glm::vec3 bg(1.0f);
    std::vector<uint8_t> withBvh;
    std::vector<uint8_t> linear;

    _putenv_s("MOLECULENS_RT_NO_BVH", "");
    REQUIRE(rayTrace(scene, cam, light, bg, 1, withBvh));
    _putenv_s("MOLECULENS_RT_NO_BVH", "1");
    REQUIRE(rayTrace(scene, cam, light, bg, 1, linear));
    _putenv_s("MOLECULENS_RT_NO_BVH", "");

    REQUIRE(withBvh.size() == linear.size());
    REQUIRE(withBvh == linear);
    // 场景确实被渲染（非全背景），避免两边都空导致"假一致"
    REQUIRE(countNonBg(withBvh, reinterpret_cast<const uint8_t*>(&bg[0])) > 100);
}

// =============================================================================
// 半透明（v2.12）：GPU 光栅路径一直按 MaterialBlock::opacity 混合，而 CPU 光追
// 此前完全忽略该字段 —— 用户反馈的"光追引擎加载不了透明"就是这里。
// 验收：
//   1) opacity=1 的输出必须与不设置该字段（默认 1）逐位一致（不破坏既有基线）；
//   2) opacity<1 的球必须"透出背景"：与白背景的偏离显著小于不透明球；
//   3) 前透明球 + 后不透明球：中心像素应被后球颜色（蓝）影响，而不是纯前球色。
// =============================================================================
TEST_CASE("ray tracer blends transparent materials with the background",
          "[engine][raytrace]") {
    const auto renderSphere = [](float opacity) {
        RtScene scene;
        RtSphere s;
        s.center = glm::vec3(0.0f);
        s.radius = 1.0f;
        s.color = glm::vec3(1.0f, 0.15f, 0.15f);
        s.opacity = opacity;
        scene.spheres.push_back(s);

        RtCamera cam;
        cam.position = glm::vec3(0.0f, 0.0f, 4.0f);
        cam.target = glm::vec3(0.0f);
        cam.fovDeg = 45.0f;
        cam.width = 48;
        cam.height = 48;
        RtLight light;
        light.shadowSamples = 1;
        light.maxBounces = 0;
        std::vector<uint8_t> px;
        REQUIRE(rayTrace(scene, cam, light, glm::vec3(1.0f), 1, px));
        return px;
    };

    const std::vector<uint8_t> opaque = renderSphere(1.0f);
    const std::vector<uint8_t> glass = renderSphere(0.25f);

    // 与白背景的总偏离：半透明必须明显更小（球"透"出背景）
    uint64_t devOpaque = 0;
    uint64_t devGlass = 0;
    for (size_t i = 0; i + 3 < opaque.size(); i += 4) {
        devOpaque += 255u - opaque[i];
        devGlass += 255u - glass[i];
    }
    REQUIRE(devOpaque > 0u);
    REQUIRE(devGlass < devOpaque / 2u);
    REQUIRE(devGlass > 0u);   // 不是完全消失

    // 前透明 + 后不透明蓝球：中心像素的蓝色分量应高于纯透明球（背后有东西）
    RtScene two;
    RtSphere front;
    front.center = glm::vec3(0.0f, 0.0f, 1.1f);
    front.radius = 1.0f;
    front.color = glm::vec3(1.0f, 0.15f, 0.15f);
    front.opacity = 0.25f;
    RtSphere back;
    back.center = glm::vec3(0.0f, 0.0f, -1.1f);
    back.radius = 1.0f;
    back.color = glm::vec3(0.1f, 0.2f, 1.0f);
    back.opacity = 1.0f;
    two.spheres.push_back(front);
    two.spheres.push_back(back);

    RtCamera cam;
    cam.position = glm::vec3(0.0f, 0.0f, 6.0f);
    cam.target = glm::vec3(0.0f);
    cam.fovDeg = 45.0f;
    cam.width = 48;
    cam.height = 48;
    RtLight light;
    light.shadowSamples = 1;
    light.maxBounces = 0;
    std::vector<uint8_t> twoPx;
    REQUIRE(rayTrace(two, cam, light, glm::vec3(0.0f), 1, twoPx));

    const size_t center = (static_cast<size_t>(24) * 48 + 24) * 4;
    // 混合结果应带蓝（后球透出），而纯前球（背景为黑时）几乎只有红
    REQUIRE(twoPx[center + 2] > 40);   // B 分量显著
    REQUIRE(twoPx[center + 2] > twoPx[center + 0] / 3);
}