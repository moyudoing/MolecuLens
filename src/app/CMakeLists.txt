# =============================================================================
# MolecuLens · src/app —— 应用（组合根 + 事件 + 控制器 + 视图 + Python + RPC）
# 规格来源：M0 工程脚手架 §4.3 / 主方案 §3.3
# =============================================================================

add_executable(moleculens_app WIN32
    main.cpp
    shell/application.h shell/application.cpp
    events/event.h events/event_bus.h events/event_bus.cpp
    controller/scene/scene_controller.h controller/scene/scene_controller.cpp
    controller/render/render_controller.h controller/render/render_controller.cpp
    controller/rep/representation_controller.h controller/rep/representation_controller.cpp
    controller/selection/selection_controller.h controller/selection/selection_controller.cpp
    controller/timeline/timeline_controller.h controller/timeline/timeline_controller.cpp
    controller/camera/camera_controller.h controller/camera/camera_controller.cpp
    controller/io/io_controller.h controller/io/io_controller.cpp
    controller/script/script_controller.h controller/script/script_controller.cpp
    data/data_manager.h data/data_manager.cpp
    view/control/view_control.h view/control/view_control.cpp
    view/container/main_view.h view/container/main_view.cpp
    view/tool/base_tool_view.h view/tool/tool_registry.h
    view/theme/ui_theme.h view/theme/ui_theme.cpp
    view/viewport/mol_scene.h view/viewport/mol_scene.cpp
    view/timeline/time_axis.h view/timeline/time_axis.cpp
    view/widget/loading_bar.h view/widget/section_header.h view/widget/badge.h
    view/tools/mol_list/mol_list_tool.h view/tools/mol_list/mol_list_tool.cpp
    view/tools/styles/styles_tool.h view/tools/styles/styles_tool.cpp
    view/tools/material/material_tool.h view/tools/material/material_tool.cpp
    view/tools/export/export_img_tool.h view/tools/export/export_img_tool.cpp
    view/tools/paint/painting_tool.h view/tools/paint/painting_tool.cpp
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

if(MOLECULENS_BUILD_PYTHON)
    target_sources(moleculens_app PRIVATE python/module/pybind_module.cpp)
    target_link_libraries(moleculens_app PRIVATE Python3::Python pybind11::module)
endif()

if(MOLECULENS_ENABLE_ZMQ)
    target_sources(moleculens_app PRIVATE rpc/server/rpc_server.cpp rpc/methods/rpc_methods.cpp)
    target_link_libraries(moleculens_app PRIVATE cppzmq)
endif()

# MSVC 下链接 winmm（glfw 音频需求）
if(WIN32)
    target_link_libraries(moleculens_app PRIVATE winmm)
    # Debug 构建生成 MAP 文件（崩溃地址定位，主方案 §14.5 崩溃处理）
    target_link_options(moleculens_app PRIVATE
        /MAP:$<TARGET_FILE_DIR:moleculens_app>/moleculens_app.map)
endif()

# —— 资源后处理（拷贝到可执行目录；主方案 §7.6 字体）——
# demo.pdb（演示数据）
add_custom_command(TARGET moleculens_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:moleculens_app>/data
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/data/demo.pdb
        $<TARGET_FILE_DIR:moleculens_app>/data/demo.pdb
    COMMENT "Copying demo data")
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
