# =============================================================================
# MolecuLens · cmake/shaders.cmake —— 着色器编译内嵌（SPIR-V → C++ 头）
# 规格来源：M0 工程脚手架 §3.3 / 主方案 §14.1
# 实现：configure 期生成（glslangValidator → spv_to_header.py 立即执行），
#       头文件在 configure 完成时即存在——VS/Ninja 生成器均可靠（规避
#       VS 生成器对 custom command 输出头的依赖图问题）。
# =============================================================================

find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(PYTHON3_EXE NAMES python3 python REQUIRED)

function(moleculens_add_shader target src out_header)
    get_filename_component(_out_dir "${out_header}" DIRECTORY)
    file(MAKE_DIRECTORY "${_out_dir}")

    # 共享 GLSL 头（global_ubo.glsl / material_shading.glsl）所在目录 = 源文件目录。
    # 经 GL_GOOGLE_include_directive 支持 #include —— 全局 UBO 布局与光照/雾公式
    # 收敛为单一定义，10 个着色器不再各自手抄一份 block
    #（主方案 §12 风险"UBO 字段与 GLSL 布局错位"由此从根上消除）。
    get_filename_component(_src_dir "${src}" DIRECTORY)

    execute_process(
        COMMAND ${GLSLANG_VALIDATOR} -V -I${_src_dir} ${src} -o ${out_header}.spv
        RESULT_VARIABLE _glsl_result
        OUTPUT_VARIABLE _glsl_out ERROR_VARIABLE _glsl_out)
    if(NOT _glsl_result EQUAL 0)
        message(FATAL_ERROR "glslangValidator failed on ${src}:\n${_glsl_out}")
    endif()

    execute_process(
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/cmake/spv_to_header.py
                ${out_header}.spv ${out_header}
        RESULT_VARIABLE _py_result
        OUTPUT_VARIABLE _py_out ERROR_VARIABLE _py_out)
    if(NOT _py_result EQUAL 0)
        message(FATAL_ERROR "spv_to_header.py failed on ${src}:\n${_py_out}")
    endif()

    message(STATUS "Shader embedded: ${src}")
    target_sources(${target} PRIVATE ${out_header})
endfunction()
