// =============================================================================
// MolecuLens · cli · main.cpp —— 命令行入口
// 规格来源：主方案 §11.3（--version/--help；退出码 0 成功 / 1 解析失败 /
//           2 渲染失败 / 3 用法错误）
// =============================================================================

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include <CLI/CLI.hpp>

#include "src/cli/cmd/convert/convert_cmd.h"
#include "src/cli/cmd/info/info_cmd.h"
#include "src/cli/cmd/render/render_cmd.h"

#include "spdlog/spdlog.h"

namespace {

constexpr const char* kVersion = "0.1.0";

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Windows 控制台默认 GBK 代码页：输出 UTF-8（源文件 /utf-8）会乱码，
    // 统一切到 UTF-8 控制台（Win10 1903+ 支持）
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    // 静默 spdlog（CLI 默认仅 stderr 错误）
    spdlog::set_level(spdlog::level::warn);

    CLI::App app{"MolecuLens CLI - 分子结构与轨迹无头工具"};
    app.set_version_flag("--version", std::string("MolecuLens CLI ") + kVersion);
    app.require_subcommand();

    moleculens::cli::renderCommand(app);
    moleculens::cli::convertCommand(app);
    moleculens::cli::infoCommand(app);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // 帮助/版本：正常退出（CLI11 自行输出；Version 异常名为 CallForVersion）
        if (e.get_name() == "Help" || e.get_name() == "HelpAll" ||
            e.get_name() == "Version" || e.get_name() == "CallForVersion") {
            return app.exit(e);
        }
        // 用法错误（缺子命令/缺必需参数/未知选项）→ 退出码 3（主方案 §11.3）
        if (e.get_name() == "RequiredError" && argc <= 1) {
            // 无参数运行：打印简短用法指引
            std::cout << app.help();
        }
        return 3;
    }
    return 0;
}
