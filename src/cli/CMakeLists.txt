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

# 字体资源：拷贝到可执行目录 font/NotoSansSC-Medium-subset.ttf（ESP 导出叠加的 TTF 文字用；缺失时回退位图字体）
if(EXISTS ${MOLECULENS_FONT_SRC})
    add_custom_command(TARGET moleculens-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:moleculens-cli>/font
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MOLECULENS_FONT_SRC}
            $<TARGET_FILE_DIR:moleculens-cli>/font/NotoSansSC-Medium-subset.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/data/font/OFL.txt
            $<TARGET_FILE_DIR:moleculens-cli>/font/OFL.txt
        COMMENT "Copying Noto Sans SC subset font + OFL.txt for CLI overlay")
endif()
