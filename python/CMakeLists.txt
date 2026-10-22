# =============================================================================
# MolecuLens · python · CMakeLists.txt —— Python 绑定（MOLECULENS_BUILD_PYTHON=ON）
# 规格来源：主方案 §11.1（导出 Python 模块）
#
# 产物布局（Release 固定为 build/python，方便 pytest/CTest 与文档统一）：
#   <build>/python/moleculens/__init__.py     ← 源文件拷贝
#   <build>/python/moleculens/_native.pyd     ← C++ 扩展（Windows；Linux 为 .so）
#
# 只链接 moleculens_entity：绑定不引入 Vulkan / GLFW，无需图形设备即可测试。
# =============================================================================

pybind11_add_module(_native MODULE
    module/pybind_module.cpp
)
target_link_libraries(_native PRIVATE moleculens_entity)
target_compile_definitions(_native PRIVATE
    MOLECULENS_PY_VERSION="${PROJECT_VERSION}")

get_property(_moleculens_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_moleculens_release_out "${CMAKE_BINARY_DIR}/python/moleculens")

# 仅 Release 使用稳定的 build/python。Debug 等其他配置放到 build/python-<config>：
# Windows 的 CPython 通常没有 python39_d.lib，且调试 CRT 的扩展不应覆盖 Release
# 产物（否则 Release 测试会悄悄导入 Debug 模块）。
if(_moleculens_multi_config)
    set(_moleculens_default_out "${_moleculens_release_out}")
    set(_moleculens_test_root "${CMAKE_BINARY_DIR}/python")
else()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(_moleculens_default_out "${_moleculens_release_out}")
        set(_moleculens_test_root "${CMAKE_BINARY_DIR}/python")
    else()
        string(TOLOWER "${CMAKE_BUILD_TYPE}" _moleculens_cfg)
        set(_moleculens_default_root "${CMAKE_BINARY_DIR}/python-${_moleculens_cfg}")
        set(_moleculens_default_out "${_moleculens_default_root}/moleculens")
        set(_moleculens_test_root "${_moleculens_default_root}")
    endif()
endif()

set_target_properties(_native PROPERTIES
    OUTPUT_NAME "_native"
    LIBRARY_OUTPUT_DIRECTORY "${_moleculens_default_out}"
    RUNTIME_OUTPUT_DIRECTORY "${_moleculens_default_out}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${_moleculens_release_out}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_moleculens_release_out}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/python-debug/moleculens"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/python-debug/moleculens"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/python-relwithdebinfo/moleculens"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/python-relwithdebinfo/moleculens"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/python-minsizerel/moleculens"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/python-minsizerel/moleculens")

file(GLOB _moleculens_py_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/moleculens/*.py")

add_custom_command(TARGET _native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${_moleculens_py_sources}
        "$<TARGET_FILE_DIR:_native>"
    COMMENT "Copying moleculens package files next to _native"
)

# Windows：把 spdlog / fmt 等运行期 DLL 拷到扩展旁边，
# 避免 `import moleculens` 报 "DLL load failed"。
if(WIN32)
    add_custom_command(TARGET _native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:_native> "$<TARGET_FILE_DIR:_native>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying Python binding runtime DLLs")
endif()

# 把 pytest 套件挂进 CTest（与 moleculens_tests 同一入口）；未安装 pytest 时
# 只提示、不阻塞工程配置与其余测试。
if(MOLECULENS_BUILD_TESTS)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m pytest --version
        RESULT_VARIABLE _moleculens_pytest_rc
        OUTPUT_QUIET ERROR_QUIET)
    if(_moleculens_pytest_rc EQUAL 0)
        add_test(NAME moleculens_python
            COMMAND "${Python3_EXECUTABLE}" -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/tests" -q)
        set_tests_properties(moleculens_python PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "MOLECULENS_PYTHON_DIR=${_moleculens_test_root}")
    else()
        message(STATUS
            "pytest 未安装（${Python3_EXECUTABLE}）；Python 绑定测试未注册到 CTest")
    endif()
endif()
