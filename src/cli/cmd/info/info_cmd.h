// =============================================================================
// MolecuLens · cli · cmd/info/info_cmd.h —— 文件信息子命令
// 规格来源：主方案 §11.3（moleculens-cli info input.gro）
// =============================================================================

#pragma once

#include <CLI/CLI.hpp>

namespace moleculens::cli {

int infoCommand(CLI::App& app);

} // namespace moleculens::cli
