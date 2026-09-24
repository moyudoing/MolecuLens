# =============================================================================
# MolecuLens · src/engine —— 渲染引擎（场景图 + 表示 + 命令流 + Vulkan 单后端 + 导出器）
# 规格来源：M0 工程脚手架 §4.2 / 主方案 §3.4
# =============================================================================

add_library(moleculens_engine STATIC
    scheduler/frame_scheduler.h scheduler/frame_scheduler.cpp
    rep/representation.h rep/representer.h rep/representer.cpp
    style/base_style.h
    style/style_registry.h style/style_registry.cpp
    style/spacefill/spacefill.h style/spacefill/spacefill.cpp
    style/ballstick/ballstick.h style/ballstick/ballstick.cpp
    style/stick/stick.h style/stick/stick.cpp
    style/wireframe/wireframe.h style/wireframe/wireframe.cpp
    style/cartoon/cartoon.h style/cartoon/cartoon.cpp
    style/licorice/licorice.h style/licorice/licorice.cpp
    style/tube/tube.h style/tube/tube.cpp
    style/ribbon/ribbon.h style/ribbon/ribbon.cpp
    style/newcartoon/newcartoon.h style/newcartoon/newcartoon.cpp
    style/surface/surface.h style/surface/surface.cpp style/surface/mc_tables.h
    style/quicksurf/quicksurf.h style/quicksurf/quicksurf.cpp
    style/quicksurf/quicksurf_avx2.cpp
    cmd/cmd.h cmd/command_list.cpp
    camera/camera.h camera/camera.cpp
    picking/picking.h picking/picking.cpp
    render/renderer.h render/renderer.cpp render/render_parameter.h
    serialize/scene_bridge.h serialize/scene_bridge.cpp
    export/tessellate/tessellate.h
    export/obj/obj_exporter.h export/obj/obj_exporter.cpp
    export/pov/pov_exporter.h export/pov/pov_exporter.cpp
    export/stl/stl_exporter.h export/stl/stl_exporter.cpp
    export/png/png_encoder.h export/png/png_encoder.cpp
    raytrace/ray_tracer.h raytrace/ray_tracer.cpp
    vulkan/geometry/geometry.h vulkan/geometry/geometry.cpp
    vulkan/backend/draw_list.h vulkan/backend/decode.h vulkan/backend/decode.cpp
    vulkan/draw/draw_geometry.h vulkan/draw/draw_geometry.cpp
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

# QuickSurf AVX2 密度核：独立 TU 用 /arch:AVX2（-mavx2）编译，其它源不受影响
if(MSVC)
    set_source_files_properties(style/quicksurf/quicksurf_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
else()
    set_source_files_properties(style/quicksurf/quicksurf_avx2.cpp
        PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()

# QuickSurf CUDA 加速（可选；需 CUDA 工具链，默认 OFF → CPU 标量/AVX2 回退）
if(MOLECULENS_QUICKSURF_CUDA)
    target_compile_definitions(moleculens_engine PUBLIC MOLECULENS_QUICKSURF_CUDA)
    target_sources(moleculens_engine PRIVATE style/quicksurf/quicksurf_cuda.cu)
    target_link_libraries(moleculens_engine PRIVATE CUDA::cudart)
endif()
target_link_libraries(moleculens_engine PUBLIC
    moleculens_entity
    glm::glm
    volk::volk
    Vulkan::Headers
    GPUOpen::VulkanMemoryAllocator
    glfw
    lodepng)
