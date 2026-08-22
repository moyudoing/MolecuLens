// =============================================================================
// MolecuLens · cli · cmd/render/render_cmd.cpp —— 无头渲染（M4 完整；当前骨架）
// =============================================================================

#include "src/cli/cmd/render/render_cmd.h"

#include "spdlog/spdlog.h"

namespace moleculens::cli {

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
    sub->add_option("--repr", repr, "表示风格 (wireframe/stick/ballstick/spacefill)");
    sub->add_option("--size", width, "宽度")->default_val(1920);
    sub->add_option("--height", height, "高度")->default_val(1080);
    sub->add_option("--camera", camera, "相机预设 (front/side/top)");
    sub->add_option("--scale", scale, "超采样倍数")->default_val(1);
    sub->add_option("--msaa", msaa, "MSAA")->default_val(4);
    sub->add_option("--bg", bg, "背景 (current/white/transparent)")->default_val("current");
    sub->add_option("--depth", depth, "位深 (8/16)")->default_val(8);
    sub->add_option("--out", out, "输出路径")->default_val("out.png");
    sub->callback([&]() {
        spdlog::info("[cli] render {} -> {} ({}x{})", input, out, width, height);
        // M4：OffscreenRenderer 直渲图像附件（无 Swapchain，主方案 §11.3）
        spdlog::warn("[cli] render: M4 落地（离屏渲染 + 图像编码）");
    });
    return 0;
}

} // namespace moleculens::cli
