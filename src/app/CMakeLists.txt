# =============================================================================
# MolecuLens · src/app —— 应用（组合根 + 事件 + 控制器 + 视图 + Python + RPC）
# 规格来源：M0 工程脚手架 §4.3 / 主方案 §3.3
# =============================================================================
# 递归收集 src/app 下全部 .cpp 源文件；CONFIGURE_DEPENDS 使新增/删除源文件时
# 自动重新配置，避免手写列表与磁盘文件脱节（当前项目已出现列表滞后）。GLOB_RECURSE
# 在生成器表达式层面将目录限定在 CMAKE_CURRENT_SOURCE_DIR，配合 glob 相对路径等效。
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS *.cpp)
# 下列子目录的源文件**不进本 target**（由专用 block 编译，或为未接入的死代码——
# 纳入会在 add_executable 与 target_sources 中重复加入（重定义），或直接编译报错）：
#   · imgui/backends       —— ImGui 官方后端，下方专用 block 编译（需集成 VK_NO_PROTOTYPES）
#   · view/tools/atom_rep  —— 未注册的遗留死代码（BaseToolView 无 2 参构造，编译不过）
# 按目录名匹配（兼容相对/绝对路径与 / 或 \ 分隔符），与重构前手写列表的取舍完全一致。
# 注：view/tools/theme 已注册（view_control 内 addModule<ThemeTool>），构造函数
#     已修正，纳入普通 GLOB 收集，不再排除。
list(FILTER SRCS EXCLUDE REGEX "imgui[\\/]backends[\\/]|view[\\/]tools[\\/]atom_rep[\\/]")
add_executable(moleculens_app WIN32
    ${SRCS}
)
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

# Python 绑定与 ZMQ RPC：规格已定（主方案 §10/§13）但源码尚未落地——
# 此前这两个分支直接引用 python/module/pybind_module.cpp 与 rpc/server/rpc_server.cpp，
# 而这两个文件在磁盘上并不存在 → 一旦开启 option 必然配置失败且原因难以定位。
# 故改为显式报错，把"未实现"讲清楚；实现落地后再接回源文件。
if(MOLECULENS_BUILD_PYTHON)
    message(FATAL_ERROR
        "MOLECULENS_BUILD_PYTHON=ON 暂不可用：Python 绑定源码（python/module/）尚未落地。"
        " 需先实现绑定模块后再开启该选项。")
endif()

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
            $<TARGET_FILE_DIR:moleculens_app>/font/OPPO_B.ttf
        COMMENT "Copying OPPO B font")
endif()
