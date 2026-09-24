// =============================================================================
// MolecuLens · cli · cmd/render/render_cmd.cpp —— 无头渲染（M4）
// 规格来源：主方案 §11.3（离屏渲染 → 图像编码；退出码 3 = 用法错误）
//
// 链路：解析 → 场景 → 命令流（Renderer::prepare，无 GPU）→ 离屏渲染
//       （OffscreenRenderer headless 自建设备）→ RGBA8 回读 → PNG。
// 说明：--depth 16 需要 16F 回读（readbackPixels 支持），但当前 PNG 编码器
//       仅支持 8-bit，故此处对 16 显式报错而不是静默降级。
// =============================================================================

#include "src/cli/cmd/render/render_cmd.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/export/png/png_encoder.h"
#include "engine/rep/representation.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::cli {

namespace {

bool parseStyle(const std::string& name, engine::RepresentationType* out) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    using RT = engine::RepresentationType;
    if (s == "wireframe" || s == "line") *out = RT::Wireframe;
    else if (s == "stick") *out = RT::Stick;
    else if (s == "ballstick" || s == "ballandstick") *out = RT::BallAndStick;
    else if (s == "spacefill" || s == "cpk") *out = RT::Spacefill;
    else if (s == "vdw") *out = RT::Vdw;
    else if (s == "cartoon") *out = RT::Cartoon;
    else if (s == "surface") *out = RT::Surface;
    else if (s == "quicksurf") *out = RT::QuickSurf;
    else if (s == "licorice") *out = RT::Licorice;
    else if (s == "tube") *out = RT::Tube;
    else if (s == "newcartoon") *out = RT::NewCartoon;
    else if (s == "ribbon") *out = RT::Ribbon;
    else return false;
    return true;
}

// 相机预设（对齐到包围球正面 / 侧面 / 俯视）
void applyCameraPreset(engine::OrbitCamera& cam, const std::string& preset) {
    std::string s = preset;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "side") {
        cam.yaw = 1.5707963f;    // 90°
        cam.pitch = 0.0f;
    } else if (s == "top") {
        cam.yaw = 0.0f;
        cam.pitch = 1.5533431f;  // 89°（避免与 up 向量退化）
    } else {                     // front（默认）
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
    }
}

// 盒式降采样（SSAA）：src 为 w*factor × h*factor
std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h,
                                uint32_t factor) {
    const uint32_t rw = w * factor;   // 源图行宽（源高为 h * factor）
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    const uint32_t n = factor * factor;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t dy = 0; dy < factor; ++dy) {
                for (uint32_t dx = 0; dx < factor; ++dx) {
                    const size_t s = (static_cast<size_t>(y * factor + dy) * rw +
                                      (x * factor + dx)) * 4;
                    if (s + 3 >= src.size()) {
                        continue;
                    }
                    r += src[s];
                    g += src[s + 1];
                    b += src[s + 2];
                    a += src[s + 3];
                }
            }
            const size_t d = (static_cast<size_t>(y) * w + x) * 4;
            out[d] = static_cast<uint8_t>(r / n);
            out[d + 1] = static_cast<uint8_t>(g / n);
            out[d + 2] = static_cast<uint8_t>(b / n);
            out[d + 3] = static_cast<uint8_t>(a / n);
        }
    }
    return out;
}

} // namespace

int renderCommand(CLI::App& app) {
    // static：callback 在 CLI11_PARSE（main）时执行，函数已返回——局部变量悬垂
    static std::string input, out = "out.png";
    static std::string repr = "ballstick";
    static int width = 1920, height = 1080;
    static std::string camera = "front";
    static int scale = 1, msaa = 4;
    static std::string bg = "current";
    static int depth = 8;

    auto* sub = app.add_subcommand("render", "无头渲染分子为图像");
    sub->add_option("input", input, "输入结构文件 (.pdb/.gro)")->required();
    sub->add_option("--repr", repr, "表示风格 (wireframe/stick/ballstick/spacefill/...)");
    sub->add_option("--size", width, "宽度")->default_val(1920);
    sub->add_option("--height", height, "高度")->default_val(1080);
    sub->add_option("--camera", camera, "相机预设 (front/side/top)");
    sub->add_option("--scale", scale, "超采样倍数（SSAA）")->default_val(1);
    sub->add_option("--msaa", msaa, "MSAA（离屏恒 1x，保留参数）")->default_val(4);
    sub->add_option("--bg", bg, "背景 (current/white/transparent)")->default_val("current");
    sub->add_option("--depth", depth, "位深 (8/16)")->default_val(8);
    sub->add_option("--out", out, "输出路径")->default_val("out.png");

    sub->callback([&]() {
        (void)msaa;   // 离屏 FBO 附件恒为 1x（管线/渲染通道一致性的硬约束）

        if (width <= 0 || height <= 0) {
            spdlog::error("[cli] render: 非法尺寸 {}x{}", width, height);
            std::exit(3);
        }
        if (depth != 8) {
            spdlog::error("[cli] render: --depth {} 暂不支持（PNG 编码器仅 8-bit）", depth);
            std::exit(3);
        }

        // —— 1. 解析 ——
        auto parsed = entity::ParserRegistry::defaultRegistry().parseFile(
            input, entity::ParseOptions{});
        if (!parsed) {
            spdlog::error("[cli] render: 解析失败: {}", parsed.errorMessage());
            std::exit(3);
        }
        if (!parsed->scene || parsed->scene->instances().empty()) {
            spdlog::error("[cli] render: 输入不含任何实例");
            std::exit(3);
        }

        // —— 2. 场景 → 命令流 ——
        engine::RepresentationType type = engine::RepresentationType::BallAndStick;
        if (!parseStyle(repr, &type)) {
            spdlog::error("[cli] render: 未知风格 '{}'", repr);
            std::exit(3);
        }
        static std::vector<engine::Representation> reps;
        engine::Representation rep;
        rep.name = "cli";
        rep.type = type;
        reps.assign(1, rep);

        engine::Renderer renderer;
        renderer.setScene(parsed->scene.get());
        renderer.bindRepresentations(&reps, 1);
        renderer.prepare();

        // 相机：自动取景后套用预设
        engine::OrbitCamera cam;
        glm::vec3 center(0.0f);
        float radius = 1.0f;
        if (renderer.sceneBounds(&center, &radius)) {
            cam.centerOn(center, radius);
        }
        applyCameraPreset(cam, camera);

        // —— 3. 离屏渲染（headless 自建设备）——
        const int ss = std::max(1, scale);
        const uint32_t rw = static_cast<uint32_t>(width * ss);
        const uint32_t rh = static_cast<uint32_t>(height * ss);

        engine::RenderConfig cfg;
        cfg.debug = false;
        cfg.msaaSamples = 0;
        engine::OffscreenRenderer off;
        std::string err;
        if (!off.init(cfg, rw, rh, &err)) {
            spdlog::error("[cli] render: 离屏渲染器初始化失败: {}", err);
            std::exit(3);
        }

        // 背景：white = 纯白；transparent 当前降级为白（PNG 编码器写不透明 alpha）
        const glm::vec4 bgSolid(0.05f, 0.05f, 0.08f, 1.0f);
        if (bg == "white" || bg == "transparent") {
            const float white[3] = {1.0f, 1.0f, 1.0f};
            off.setBackground(white);
        } else {
            off.setBackground(&bgSolid[0]);
        }

        engine::RenderView view = renderer.viewFor(cam, rw, rh);
        off.execute(renderer.commandList(), view);

        std::vector<uint8_t> px;
        if (!off.readbackPixels(px, engine::PixelFormat::Rgba8)) {
            spdlog::error("[cli] render: 像素回读失败");
            std::exit(3);
        }

        // —— 4. SSAA 降采样 + 编码 ——
        std::vector<uint8_t> finalPx =
            ss > 1 ? downsample(px, static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), static_cast<uint32_t>(ss))
                   : px;
        if (!engine::writePng(out, static_cast<uint32_t>(width),
                              static_cast<uint32_t>(height), finalPx)) {
            spdlog::error("[cli] render: PNG 写出失败: {}", out);
            std::exit(3);
        }
        spdlog::info("[cli] render: {} -> {} ({}x{}, ssaa={})", input, out, width, height,
                     ss);
    });
    return 0;
}

} // namespace moleculens::cli
