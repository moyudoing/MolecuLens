// =============================================================================
// MolecuLens · tests · vulkan/offscreen_smoke.cpp —— 离屏渲染冒烟
// 规格来源：主方案 §14.3（加载夹具 → 渲染一帧 → 校验非空图像 + 像素统计；
//           无窗口环境；无 GPU 时跳过）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>

#include "engine/cmd/cmd.h"
#include "engine/rep/representation.h"
#include "engine/render/renderer.h"   // RenderView
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

// 注意三者都要：`using namespace moleculens;` 让 `engine::` / `entity::` 这两个
// **限定名**可用（只 using 子命名空间拿到的是其成员，不含子命名空间本身的名字）。
using namespace moleculens;
using namespace moleculens::engine;
using namespace moleculens::entity;

TEST_CASE("OffscreenRenderer renders and reads back non-empty pixels", "[vulkan][smoke]") {
    RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    OffscreenRenderer renderer;
    std::string err;
    if (!renderer.init(cfg, 128, 96, &err)) {
        WARN("Skipping offscreen smoke (no Vulkan device): " << err);
        return;
    }

    // 构造简单命令流：一个球
    SceneCommandList list;
    MaterialBlock mat;
    mat.ambient = glm::vec4(0.6f, 0.6f, 0.6f, 1);
    mat.diffuse = glm::vec4(0.9f, 0.9f, 0.9f, 1);
    mat.specular = glm::vec4(0.3f, 0.3f, 0.3f, 1);
    mat.shininess = 32.0f;
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));
    SphereInst inst;
    inst.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 10.0f);
    inst.color = glm::vec4(1.0f, 0.2f, 0.2f, 1.0f);
    inst.pickId = glm::uvec2(1, 0);
    inst.pad = glm::uvec2(0, 0);
    list.append(Cmd::SphereInst, &inst, sizeof(inst));

    RenderView view;
    view.camera.centerOn({0, 0, 0}, 10.0f);
    view.width = 128;
    view.height = 96;
    renderer.execute(list, view);

    std::vector<uint8_t> pixels;
    REQUIRE(renderer.readbackPixels(pixels, PixelFormat::Rgba8));
    REQUIRE(pixels.size() == 128u * 96u * 4u);

    // 像素统计：应有大量非背景像素（球投影）。
    // 背景参考 = 左上角像素（几何恒居中，四角必为背景）——不用绝对阈值：离屏颜色
    // 附件自 P6 起为 _SRGB（写时编码），背景字节值上移，固定阈值会失去判别力。
    const uint8_t* bgp = pixels.data();
    uint32_t nonBg = 0;
    uint32_t reddish = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const bool isBg = std::abs(static_cast<int>(pixels[i]) - bgp[0]) <= 24 &&
                          std::abs(static_cast<int>(pixels[i + 1]) - bgp[1]) <= 24 &&
                          std::abs(static_cast<int>(pixels[i + 2]) - bgp[2]) <= 24;
        if (!isBg) {
            ++nonBg;
        }
        // 球色为红色调（R > B + 30；照明后红球仍保持色相）
        if (pixels[i] > pixels[i + 2] + 30 && pixels[i] > 50) {
            ++reddish;
        }
    }
    REQUIRE(nonBg > 100);      // 球占据视口大部分
    REQUIRE(reddish > 100);    // 存在大量红球像素（光照下 R 通道占优）
}

// 方向光阴影（主方案 §6.6.4）：同一几何/相机/光矩阵，只切换阴影强度做 A/B ——
//   · w = 0  → 着色器直接返回 1.0（无阴影基线）
//   · w > 0  → 3×3 PCF 查表
// 断言：两图必须不同、整体变暗，且**存在完全未变的像素**（受光区不应被影响）
// —— 后者把"真的投影"与"整体压暗"区分开。
TEST_CASE("Directional shadows darken only the occluded region", "[vulkan][smoke]") {
    // 场景：两个原子沿光源方向排布，近光者把远光者挡住。
    // 光矩阵由 Renderer::sceneLighting 按包围球拟合，故必须让**两个原子都在场景里**
    // （否则包围球只覆盖一个，另一个落在光源视锥外 → 不投影）。
    entity::SceneStructure scene;
    const glm::vec3 L = glm::normalize(glm::vec3(0.40f, 0.80f, 0.60f));
    entity::MoleculeStructure ms;
    for (int i = 0; i < 2; ++i) {
        entity::Atom a;
        a.element = entity::ElementType::Carbon;
        ms.atoms.push_back(a);
        ms.elements.push_back(entity::ElementType::Carbon);
        ms.atomNameIds.push_back(0);
    }
    ms.key = entity::computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<entity::Trajectory>();
    entity::Timestep ts;
    ts.positions = {glm::vec3(0.0f), L * 3.0f};
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, entity::Transform{}, std::move(traj));

    engine::Representation rep;
    rep.type = engine::RepresentationType::Spacefill;   // 球 → 会投影
    std::vector<engine::Representation> reps{rep};

    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();

    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    engine::OffscreenRenderer off;
    std::string err;
    if (!off.init(cfg, 128, 96, &err)) {
        WARN("Skipping shadow test (no Vulkan device): " << err);
        return;
    }
    if (!off.shadowMapAvailable()) {
        WARN("Device has no usable shadow map; skipping");
        return;
    }

    glm::vec3 center(0.0f);
    float radius = 1.0f;
    REQUIRE(renderer.sceneBounds(&center, &radius));
    engine::OrbitCamera cam;
    cam.centerOn(center, radius);

    engine::RenderView view = renderer.viewFor(cam, 128, 96);
    // 光矩阵必须有内容（Renderer::sceneLighting 按包围球拟合），否则着色器会
    // 因 uv 越界而永远返回"受光" —— 那样这个测试会假通过。
    REQUIRE(view.light.lightViewProj != glm::mat4(0.0f));

    const auto renderWith = [&](float shadowStrength) {
        engine::RenderView v = view;
        v.light.shadowStrength = shadowStrength;
        off.execute(renderer.commandList(), v);
        std::vector<uint8_t> px;
        REQUIRE(off.readbackPixels(px, engine::PixelFormat::Rgba8));
        return px;
    };

    const std::vector<uint8_t> noShadow = renderWith(0.0f);
    const std::vector<uint8_t> withShadow = renderWith(0.9f);
    REQUIRE(noShadow.size() == withShadow.size());

    uint64_t sumA = 0, sumB = 0;
    uint32_t changed = 0, unchanged = 0;
    for (size_t i = 0; i + 3 < noShadow.size(); i += 4) {
        const int a = noShadow[i] + noShadow[i + 1] + noShadow[i + 2];
        const int b = withShadow[i] + withShadow[i + 1] + withShadow[i + 2];
        sumA += static_cast<uint64_t>(a);
        sumB += static_cast<uint64_t>(b);
        if (std::abs(a - b) > 6) {
            ++changed;
        } else {
            ++unchanged;
        }
    }
    // ① 阴影真的改变了画面
    REQUIRE(changed > 0);
    // ② 整体变暗（不是变亮、也不是无变化）
    REQUIRE(sumB < sumA);
    // ③ 存在明显未受影响的区域（受光面/背景）——排除"整幅被均匀压暗"
    REQUIRE(unchanged > changed);
}

// 颜色附件格式可切换（16F ⇄ 8-bit SRGB）：切换会重建渲染通道 + 全部管线 + FBO
// —— 这是本项目历史上"空白屏"的高发区（渲染通道被销毁而管线仍引用它）。
// 故此处来回切换多次，并断言**每次切回 8-bit 后渲染结果与首次完全一致**。
TEST_CASE("OffscreenRenderer survives colour attachment format toggling",
          "[vulkan][smoke]") {
    RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    OffscreenRenderer renderer;
    std::string err;
    if (!renderer.init(cfg, 64, 48, &err)) {
        WARN("Skipping attachment-toggle test (no Vulkan device): " << err);
        return;
    }

    SceneCommandList list;
    MaterialBlock mat;
    mat.diffuse = glm::vec4(0.9f, 0.9f, 0.9f, 1);
    mat.opacity = 1.0f;
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));
    SphereInst inst;
    inst.posRadius = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    inst.color = glm::vec4(0.9f, 0.4f, 0.2f, 1.0f);
    list.append(Cmd::SphereInst, &inst, sizeof(inst));

    RenderView view;
    view.camera.centerOn(glm::vec3(0.0f), 1.0f);
    view.width = 64;
    view.height = 48;

    REQUIRE(renderer.colorAttachment() == ColorAttachment::Rgba8Srgb);
    renderer.execute(list, view);
    std::vector<uint8_t> reference;
    REQUIRE(renderer.readbackPixels(reference, PixelFormat::Rgba8));
    REQUIRE(reference.size() == 64u * 48u * 4u);

    // 回读格式与附件不一致必须失败（而不是拷贝出脏数据）
    std::vector<uint8_t> wrong;
    REQUIRE_FALSE(renderer.readbackPixels(wrong, PixelFormat::Rgba16F));

    if (!renderer.setColorAttachment(ColorAttachment::Rgba16Float)) {
        WARN("Device lacks 16F colour attachment; skipping the 16F half of the toggle");
        return;
    }
    REQUIRE(renderer.colorAttachment() == ColorAttachment::Rgba16Float);
    // 16F 附件下按 8-bit 回读同样必须失败
    REQUIRE_FALSE(renderer.readbackPixels(wrong, PixelFormat::Rgba8));
    renderer.execute(list, view);
    std::vector<uint8_t> halfBytes;
    REQUIRE(renderer.readbackPixels(halfBytes, PixelFormat::Rgba16F));
    REQUIRE(halfBytes.size() == 64u * 48u * 8u);

    // 切回 8-bit：渲染通道/管线/FBO 已重建，结果必须与首次逐字节相同
    REQUIRE(renderer.setColorAttachment(ColorAttachment::Rgba8Srgb));
    renderer.execute(list, view);
    std::vector<uint8_t> again;
    REQUIRE(renderer.readbackPixels(again, PixelFormat::Rgba8));
    REQUIRE(again == reference);
}
