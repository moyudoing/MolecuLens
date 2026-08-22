// =============================================================================
// MolecuLens · cli · cmd/info/info_cmd.cpp —— 文件信息
// 规格来源：主方案 §11.3（解析并打印原子/键/帧统计；退出码 1 解析失败）
// =============================================================================

#include "src/cli/cmd/info/info_cmd.h"

#include <cstdio>

#include "entity/scene/scene.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::cli {

int infoCommand(CLI::App& app) {
    // static：callback 在 CLI11_PARSE（main）时执行，函数已返回——局部变量悬垂
    static std::string input;
    auto* sub = app.add_subcommand("info", "打印结构文件信息");
    sub->add_option("input", input, "输入文件")->required();
    sub->callback([&]() {
        auto result = entity::ParserRegistry::defaultRegistry().parseFile(
            input, entity::ParseOptions{});
        if (!result) {
            spdlog::error("[cli] parse failed: {}", result.errorMessage());
            return;
        }
        const auto& scene = result->scene;
        if (!scene || scene->instances().empty()) {
            spdlog::error("[cli] no instances");
            return;
        }
        const auto& inst = scene->instances()[0];
        const auto& ms = scene->structures()[inst.structureIndex];
        std::printf("文件: %s\n", input.c_str());
        std::printf("实例数: %zu\n", scene->instances().size());
        std::printf("原子数: %zu\n", ms.elements.size());
        std::printf("键数: %zu\n", ms.bonds.size());
        std::printf("帧数: %llu\n", result->frameCount);
        if (!result->warnings.empty()) {
            std::printf("警告: %zu 条\n", result->warnings.size());
            for (const auto& w : result->warnings) {
                std::printf("  行 %zu: %s\n", w.line, w.message.c_str());
            }
        }
    });
    return 0;
}

} // namespace moleculens::cli
