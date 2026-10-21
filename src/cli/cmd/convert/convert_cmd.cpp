// =============================================================================
// MolecuLens · cli · cmd/convert/convert_cmd.cpp —— 格式转换
// 规格来源：主方案 §11.3（moleculens-cli convert input output）
//
// 支持：.obj（含 .mtl）/ .stl / .pov / .mlscene。
//   · .obj/.stl/.pov —— 由命令流经对应导出器写出
//   · .mlscene       —— 场景快照（SceneStructure → SceneSnapshot，engine 侧桥接）
// 说明：以上都**不需要 GPU**：Renderer::prepare() 只做"场景 → 命令流"，
//       不触碰任何 Vk* 资源，故 CLI 可在无窗口/无设备环境完整跑通导出链路。
// =============================================================================

#include "src/cli/cmd/convert/convert_cmd.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/export/obj/obj_exporter.h"
#include "engine/export/pov/pov_exporter.h"
#include "engine/export/stl/stl_exporter.h"
#include "engine/rep/representation.h"
#include "engine/render/renderer.h"
#include "engine/serialize/scene_bridge.h"   // .mlscene：SceneStructure → SceneSnapshot
#include "entity/scene/scene.h"
#include "persist/mlscene/mlscene.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::cli {

namespace {

// 风格名 → 表示类型（接受英文与中文名，大小写不敏感）
bool parseStyle(const std::string& name, engine::RepresentationType* out) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    using RT = engine::RepresentationType;
    if (s == "wireframe" || s == "line" || name == "线状") *out = RT::Wireframe;
    else if (s == "stick" || name == "棍状") *out = RT::Stick;
    else if (s == "ballstick" || s == "ballandstick" || name == "球棍")
        *out = RT::BallAndStick;
    else if (s == "spacefill" || s == "cpk" || name == "空间填充" || name == "CPK")
        *out = RT::Spacefill;
    else if (s == "vdw") *out = RT::Vdw;
    else if (s == "cartoon" || name == "卡通") *out = RT::Cartoon;
    else if (s == "surface" || name == "表面") *out = RT::Surface;
    else if (s == "quicksurf") *out = RT::QuickSurf;
    else if (s == "licorice" || name == "甘草棍") *out = RT::Licorice;
    else if (s == "tube" || name == "管状") *out = RT::Tube;
    else if (s == "newcartoon") *out = RT::NewCartoon;
    else if (s == "ribbon" || name == "丝带") *out = RT::Ribbon;
    else if (s == "plate" || name == "平板") *out = RT::Plate;
    else if (s == "diamond" || s == "gem" || name == "菱形") *out = RT::Diamond;
    else if (s == "dotcloud" || s == "dots" || name == "点云") *out = RT::DotCloud;
    else return false;
    return true;
}

} // namespace

int convertCommand(CLI::App& app) {
    // static：callback 在 CLI11_PARSE（main）时执行，函数已返回——局部变量悬垂
    static std::string input, out;
    static std::string style = "ballstick";

    auto* sub = app.add_subcommand("convert", "转换结构文件格式（.obj/.stl/.pov/.mlscene）");
    sub->add_option("input", input, "输入文件（.pdb/.gro/.sdf）")->required();
    sub->add_option("output", out, "输出文件（.obj/.stl/.pov/.mlscene）")->required();
    sub->add_option("--repr", style, "表示风格（影响导出的几何）")
        ->default_val("ballstick");

    sub->callback([&]() {
        namespace fs = std::filesystem;

        // —— 1. 解析输入 ——
        auto parsed = entity::ParserRegistry::defaultRegistry().parseFile(
            input, entity::ParseOptions{});
        if (!parsed) {
            spdlog::error("[cli] convert: 解析失败: {}", parsed.errorMessage());
            std::exit(3);
        }
        if (!parsed->scene || parsed->scene->instances().empty()) {
            spdlog::error("[cli] convert: 输入不含任何实例");
            std::exit(3);
        }

        // —— 2. 场景 → 命令流（无 GPU；prepare 只构建设备无关命令）——
        engine::RepresentationType type = engine::RepresentationType::BallAndStick;
        if (!parseStyle(style, &type)) {
            spdlog::error("[cli] convert: 未知风格 '{}'", style);
            std::exit(3);
        }
        // 表示数据须在 Renderer 使用期间保持有效
        static std::vector<engine::Representation> reps;
        engine::Representation rep;
        rep.name = "cli";
        rep.type = type;
        reps.assign(1, rep);

        engine::Renderer renderer;
        renderer.setScene(parsed->scene.get());
        renderer.bindRepresentations(&reps, 1);
        renderer.prepare();

        // —— 3. 按扩展名导出 ——
        std::string ext = out;
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto endsWith = [&ext](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return ext.size() >= n && ext.compare(ext.size() - n, n, suffix) == 0;
        };

        entity::Result<void> rc = entity::Result<void>::error(
            entity::ErrorCode::FormatError, "unsupported output format");
        if (endsWith(".obj")) {
            rc = engine::ObjExporter::exportScene(renderer.commandList(), out);
        } else if (endsWith(".stl")) {
            rc = engine::StlExporter::exportScene(renderer.commandList(), out);
        } else if (endsWith(".pov")) {
            rc = engine::PovExporter::exportScene(renderer.commandList(), out);
        } else if (endsWith(".mlscene")) {
            // 场景快照：SceneStructure → SceneSnapshot（engine 侧桥接）→ 序列化。
            // 命令流段由桥接从 commandStream 取出（此前 save() 内新建空命令流，
            // 导致该段永远存不进文件）。相机不在 CLI 语义内 → 留空用快照默认值。
            engine::SnapshotSource src;
            src.scene = parsed->scene.get();
            src.representations = &reps;
            src.commandStream = &renderer.commandList();
            const persist::mlscene::SceneSnapshot snap = engine::makeSnapshot(src);
            auto sres = persist::mlscene::MlSceneSerializer::save(snap, out);
            if (!sres) {
                spdlog::error("[cli] convert: .mlscene 写出失败: {}",
                              sres.errorMessage());
                std::exit(3);
            }
            spdlog::info("[cli] convert: {} -> {} 完成（场景快照）", input, out);
            return;
        } else {
            spdlog::error("[cli] convert: 不支持的输出格式 '{}'（支持 .obj/.stl/.pov/"
                          ".mlscene）",
                          out);
            std::exit(3);
        }

        if (!rc) {
            spdlog::error("[cli] convert: 写出失败: {}", rc.errorMessage());
            std::exit(3);
        }
        spdlog::info("[cli] convert: {} -> {} 完成", input, out);
    });
    return 0;
}

} // namespace moleculens::cli
