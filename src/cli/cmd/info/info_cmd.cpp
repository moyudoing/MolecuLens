// =============================================================================
// MolecuLens · cli · cmd/info/info_cmd.cpp —— 文件信息
// 规格来源：主方案 §11.3（解析并打印原子/键/帧统计；退出码 1 解析失败）
// =============================================================================

#include "src/cli/cmd/info/info_cmd.h"

#include <cstdio>
#include <memory>

#include "entity/scene/scene.h"
#include "entity/trajectory/dcd_reader.h"
#include "entity/trajectory/trajectory_reader.h"
#include "entity/trajectory/trr/trr_reader.h"
#include "entity/trajectory/xtc/xtc_reader.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::cli {

int infoCommand(CLI::App& app) {
    // static：callback 在 CLI11_PARSE（main）时执行，函数已返回——局部变量悬垂
    static std::string input;
    auto* sub = app.add_subcommand("info", "打印结构文件信息");
    sub->add_option("input", input, "输入文件")->required();
    sub->callback([&]() {
        // 轨迹文件（.dcd/.xtc）只有坐标、没有拓扑：直接报告帧数/原子数/时间步，
        // 而不是丢给结构解析器报 "No parser found"。
        {
            std::unique_ptr<entity::ITrajectoryReader> tr;
            entity::DcdReader dcd;
            entity::XtcReader xtc;
            entity::TrrReader trr;
            if (dcd.canOpen(input)) {
                tr = std::make_unique<entity::DcdReader>();
            } else if (xtc.canOpen(input)) {
                tr = std::make_unique<entity::XtcReader>();
            } else if (trr.canOpen(input)) {
                tr = std::make_unique<entity::TrrReader>();
            }
            if (tr) {
                auto opened = tr->open(input);
                if (!opened) {
                    spdlog::error("[cli] trajectory open failed: {}",
                                  opened.errorMessage());
                    return;
                }
                std::printf("文件: %s\n", input.c_str());
                std::printf("格式: 轨迹 (%s)\n", tr->name());
                std::printf("原子数: %u\n", tr->atomCount());
                std::printf("帧数: %llu\n",
                            static_cast<unsigned long long>(tr->frameCount()));
                std::printf("时间步: %.3f ps\n", tr->dt());
                return;
            }
        }
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
        // 原子/键总数按**实例**累加：GRO 的每个分子是一个实例，结构池按
        // StructureKey 去重（2032 个水分子共享一个结构），只看首实例/结构池
        // 会把 md.gro 的 6452 原子报成 30（首个溶质分子的原子数）。
        size_t totalAtoms = 0;
        size_t totalBonds = 0;
        for (const auto& in : scene->instances()) {
            if (in.structureIndex >= scene->structures().size()) {
                continue;
            }
            const auto& m = scene->structures()[in.structureIndex];
            totalAtoms += m.elements.size();
            totalBonds += m.bonds.size();
        }
        std::printf("文件: %s\n", input.c_str());
        std::printf("结构数: %zu\n", scene->structures().size());
        std::printf("实例数: %zu\n", scene->instances().size());
        std::printf("原子数: %zu\n", totalAtoms);
        std::printf("键数: %zu\n", totalBonds);
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
