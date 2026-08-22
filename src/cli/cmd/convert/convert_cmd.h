// =============================================================================
// MolecuLens · cli · cmd/convert/convert_cmd.h —— 格式转换子命令
// 规格来源：主方案 §11.3（moleculens-cli convert input.gro output.mlscene）
// =============================================================================

#pragma once

#include <CLI/CLI.hpp>

namespace moleculens::cli {

int convertCommand(CLI::App& app);

} // namespace moleculens::cli
