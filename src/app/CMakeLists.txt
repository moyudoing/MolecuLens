# =============================================================================
# MolecuLens · src/app —— 应用（组合根 + 事件 + 控制器 + 视图 + Python + RPC）
# 规格来源：M0 工程脚手架 §4.3 / 主方案 §3.3
# =============================================================================
# 递归收集 src/app 下全部 .cpp 源文件；CONFIGURE_DEPENDS 使新增/删除源文件时
# 自动重新配置，避免手写列表与磁盘文件脱节（当前项目已出现列表滞后）。GLOB_RECURSE
# 在生成器表达式层面将目录限定在 CMAKE_CURRENT_SOURCE_DIR，配合 glob 相对路径等效。
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS *.cpp)
# 唯一仍需排除的子目录：
#   · imgui/backends —— ImGui 官方后端，下方专用 block 编译（需集成 VK_NO_PROTOTYPES）；
#     纳入 GLOB 会在 add_executable 与 target_sources 中重复加入（重定义）。
# 按目录名匹配（兼容相对/绝对路径与 / 或 \ 分隔符）。
# 注 1：view/tools/theme 已注册（view_control 内 addModule<ThemeTool>），构造函数已修正，
#      纳入普通 GLOB 收集，不再排除。
# 注 2：view/tools/atom_rep 曾在此排除（未注册的遗留死代码、编译不过）。该目录与
#      engine/rep/atom/ 已整体删除 —— 原子级控制由 Representation::selection
#      （逐表示原子子集）+ 多表示管理承担，不再保留第二套未接线的实现。
list(FILTER SRCS EXCLUDE REGEX "imgui[\\/]backends[\\/]")
add_executable(moleculens_app WIN32
    ${SRCS}
)

# Windows 图标资源（app.ico：16–256 px 六档；任务栏/资源管理器/文件属性都用它）。
# 仅 MSVC 需要 .rc，MinGW 会自行忽略。
if(MSVC)
    target_sources(moleculens_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app.rc)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/app.rc PROPERTIES
        OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app.ico)
endif()
target_link_libraries(moleculens_app PRIVATE moleculens_engine spdlog::spdlog imgui::imgui)

# ImGui 官方后端：本项目使用 volk（VK_NO_PROTOTYPES 全局定义）——
# vcpkg 编译进 imgui.lib 的后端（无 VK_NO_PROTOTYPES）会把 vk* 函数调用链接到
# volk 的同名数据符号（.data），运行时 call 到数据段崩溃。
# 因此后端源码由 app 编译（继承 VK_NO_PROTOTYPES，与 volk 宏一致）。
set(_imgui_backends ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)
if(EXISTS ${_imgui_backends}/imgui_impl_glfw.cpp AND EXISTS ${_imgui_backends}/imgui_impl_vulkan.cpp)
    target_sources(moleculens_app PRIVATE
        ${_imgui_backends}/imgui_impl_glfw.cpp
        ${_imgui_backends}/imgui_impl_vulkan.cpp)
    target_include_directories(moleculens_app PRIVATE ${_imgui_backends})
else()
    message(FATAL_ERROR "imgui backends source missing at ${_imgui_backends}")
endif()

# Python 绑定已落地在仓库根目录 python/（pybind11，顶层 CMake 在
# MOLECULENS_BUILD_PYTHON=ON 时 add_subdirectory(python)），本可执行目标不再
# 承担绑定编译。ZMQ RPC 规格已定（主方案 §10/§13）但源码尚未落地——开启时显式
# 报错，避免引用不存在的 rpc/server/rpc_server.cpp 后在链接期才失败。
if(MOLECULENS_ENABLE_ZMQ)
    message(FATAL_ERROR
        "MOLECULENS_ENABLE_ZMQ=ON 暂不可用：RPC 服务源码（rpc/server/、rpc/methods/）尚未落地。"
        " 需先实现 RPC 模块后再开启该选项。")
endif()

# MSVC 下链接 winmm（glfw 音频需求）
if(WIN32)
    target_link_libraries(moleculens_app PRIVATE winmm)
    # Debug 构建生成 MAP 文件（崩溃地址定位，主方案 §14.5 崩溃处理）
    target_link_options(moleculens_app PRIVATE
        /MAP:$<TARGET_FILE_DIR:moleculens_app>/moleculens_app.map)
endif()

# —— 资源后处理（拷贝到可执行目录；源缺失不报错——避免增量构建因临时缺文件失败）——
# 数据目录（PDB / SDF / GRO）
add_custom_command(TARGET moleculens_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:moleculens_app>/data)
# EPI.pdb（演示结构）；存在才拷贝
if(EXISTS ${CMAKE_SOURCE_DIR}/data/EPI.pdb)
    add_custom_command(TARGET moleculens_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/data/EPI.pdb
            $<TARGET_FILE_DIR:moleculens_app>/data/EPI.pdb)
endif()
# md.gro（溶剂化大体系，G4 demo）；存在才拷贝
if(EXISTS ${CMAKE_SOURCE_DIR}/data/md.gro)
    add_custom_command(TARGET moleculens_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/data/md.gro
            $<TARGET_FILE_DIR:moleculens_app>/data/md.gro)
endif()
# 字体（存在才拷贝；缺失时运行期回退系统字体）
if(EXISTS ${MOLECULENS_FONT_SRC})
    add_custom_command(TARGET moleculens_app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:moleculens_app>/font
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${MOLECULENS_FONT_SRC}
            $<TARGET_FILE_DIR:moleculens_app>/font/NotoSansSC-Medium-subset.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/data/font/OFL.txt
            $<TARGET_FILE_DIR:moleculens_app>/font/OFL.txt
        COMMENT "Copying Noto Sans SC subset font + OFL.txt")
endif()
