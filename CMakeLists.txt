cmake_minimum_required(VERSION 3.21)

# —— vcpkg 探测（必须在 project() 之前设置才作为 toolchain 生效）——
# 优先本项目依赖安装的 vcpkg（F:\vcpkg / $VCPKG_ROOT），避免 CMake 自动
# 选中 VS 捆绑 vcpkg（其 registry 更新依赖 GitHub，受限网络下会失败）。
# 用户显式传入 -DCMAKE_TOOLCHAIN_FILE 时（cache 已定义）不覆盖。
if(NOT CMAKE_TOOLCHAIN_FILE)
    if(EXISTS "F:/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "F:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    elseif(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    elseif(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()

project(MolecuLens VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# —— 选项（主方案 §14.1）——
option(MOLECULENS_BUILD_APP    "Build GUI app" ON)
option(MOLECULENS_BUILD_CLI    "Build CLI" ON)
option(MOLECULENS_BUILD_PYTHON "Build python bindings" OFF)
option(MOLECULENS_BUILD_TESTS  "Build tests" ON)
option(MOLECULENS_ENABLE_ZMQ   "Enable ZMQ RPC" OFF)
option(MOLECULENS_ENABLE_PLUGINS "Enable dlopen plugins" ON)
# QuickSurf 表面：CUDA GPU 加速（需 CUDA 工具链；默认 OFF → CPU 标量/AVX2 回退）
option(MOLECULENS_QUICKSURF_CUDA "Enable CUDA acceleration for QuickSurf surface" OFF)
if(MOLECULENS_QUICKSURF_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
endif()
set(MOLECULENS_MSAA 4 CACHE STRING "MSAA samples")

# —— 字体资源（Noto Sans SC 子集，SIL OFL 1.1；生成方式见 tools/subset_font.py）——
set(MOLECULENS_FONT_SRC "${CMAKE_SOURCE_DIR}/data/font/NotoSansSC-Medium-subset.ttf")

# —— 警告等级（验收：零警告）——
if(MSVC)
    add_compile_options(/W4)
    add_compile_options(/utf-8)          # 源码 UTF-8（中文注释必需，避免 GBK 解析错乱）
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

include(cmake/deps.cmake)

add_subdirectory(src/entity)     # 实体 + IO + 持久化（moleculens_entity）
add_subdirectory(src/engine)     # 渲染引擎（含 Vulkan 单后端 + 导出器）
if(MOLECULENS_BUILD_APP)
    add_subdirectory(src/app)
endif()
if(MOLECULENS_BUILD_CLI)
    add_subdirectory(src/cli)
endif()
if(MOLECULENS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(MOLECULENS_BUILD_PYTHON)
    add_subdirectory(python)      # Python 绑定（pybind11，仅依赖 entity）
endif()
add_subdirectory(shaders)

# —— 安装规则与发布打包（cmake --install / cpack；NSIS 脚本在 installer/）——
include(cmake/packaging.cmake)
