# =============================================================================
# MolecuLens · src/cli —— 命令行（无头渲染 / 导出 / 信息）
# 规格来源：M0 工程脚手架 §4.4 / 主方案 §11.3
# =============================================================================

add_executable(moleculens-cli
    main.cpp
    cmd/render/render_cmd.h cmd/render/render_cmd.cpp
    cmd/convert/convert_cmd.h cmd/convert/convert_cmd.cpp
    cmd/info/info_cmd.h cmd/info/info_cmd.cpp
)
target_link_libraries(moleculens-cli PRIVATE moleculens_engine CLI11::CLI11)
