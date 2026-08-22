// =============================================================================
// MolecuLens · cli · cmd/render/render_cmd.h —— 无头渲染子命令
// 规格来源：主方案 §11.3（moleculens-cli render input.pdb --repr ballstick
//           --size 1920x1080 --camera front --out out.png；退出码 2 渲染失败）
// =============================================================================

#pragma once

#include <string>

#include <CLI/CLI.hpp>

namespace moleculens::cli {

int renderCommand(CLI::App& app);

} // namespace moleculens::cli
