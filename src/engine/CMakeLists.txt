# =============================================================================
# MolecuLens · src/engine —— 渲染引擎（场景图 + 表示 + 命令流 + Vulkan 单后端 + 导出器）
# 规格来源：M0 工程脚手架 §4.2 / 主方案 §3.4
# =============================================================================

add_library(moleculens_engine STATIC
    scene/scene_node.h scene/scene_graph.h
    scheduler/frame_scheduler.h scheduler/frame_scheduler.cpp
    rep/representation.h rep/representer.h rep/representer.cpp
    style/spacefill/spacefill.h style/spacefill/spacefill.cpp
    style/ballstick/ballstick.h style/ballstick/ballstick.cpp
    style/stick/stick.h style/stick/stick.cpp
    style/wireframe/wireframe.h style/wireframe/wireframe.cpp
    style/cartoon/cartoon.h style/cartoon/cartoon.cpp
    style/surface/surface.h
    style/label/label.h
    cmd/cmd.h cmd/command_list.cpp
    camera/camera.h camera/camera.cpp
    picking/picking.h picking/picking.cpp
    render/renderer.h render/renderer.cpp render/render_parameter.h
    export/obj/obj_exporter.h export/obj/obj_exporter.cpp
    export/pov/pov_exporter.h export/pov/pov_exporter.cpp
    export/stl/stl_exporter.h export/stl/stl_exporter.cpp
    vulkan/device/device.h vulkan/device/device.cpp
    vulkan/memory/buffer.h vulkan/memory/buffer.cpp vulkan/memory/vma_impl.cpp
    vulkan/pipeline/pipeline.h vulkan/pipeline/pipeline.cpp
    vulkan/renderpass/render_pass.h vulkan/renderpass/render_pass.cpp
    vulkan/desc/descriptors.h vulkan/desc/descriptors.cpp
    vulkan/realtime/vulkan_renderer.h vulkan/realtime/vulkan_renderer.cpp
    vulkan/offscreen/offscreen_renderer.h vulkan/offscreen/offscreen_renderer.cpp
)
target_include_directories(moleculens_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_BINARY_DIR})   # 生成头以 gen/shaders/xxx.h 形式包含

# Vulkan 函数经 volk 动态加载（主方案 §6.3）；VK_NO_PROTOTYPES 由 volk 要求
target_compile_definitions(moleculens_engine PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(moleculens_engine PUBLIC
    moleculens_entity
    glm::glm
    volk::volk
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    glfw)
