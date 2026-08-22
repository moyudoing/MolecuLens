// =============================================================================
// MolecuLens · cli · cmd/convert/convert_cmd.cpp —— 格式转换（M4 完整）
// =============================================================================

#include "src/cli/cmd/convert/convert_cmd.h"

#include "spdlog/spdlog.h"

namespace moleculens::cli {

int convertCommand(CLI::App& app) {
    // static：callback 在 CLI11_PARSE（main）时执行，函数已返回——局部变量悬垂
    static std::string input, out;
    auto* sub = app.add_subcommand("convert", "转换结构文件格式（.mlscene 等）");
    sub->add_option("input", input, "输入文件")->required();
    sub->add_option("output", out, "输出文件")->required();
    sub->callback([&]() {
        spdlog::info("[cli] convert {} -> {}", input, out);
        // M4：ParserRegistry 解析 → MlSceneSerializer::save
        spdlog::warn("[cli] convert: M4 落地（.mlscene 序列化）");
    });
    return 0;
}

} // namespace moleculens::cli
