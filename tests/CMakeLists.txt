# =============================================================================
# MolecuLens · tests —— 单元 / 集成测试
# 规格来源：M0 工程脚手架 §4.5 / 主方案 §14.3
# =============================================================================

add_executable(moleculens_tests
    entity/element/element_tests.cpp
    entity/identity/identity_tests.cpp
    entity/selection/selection_tests.cpp
    entity/serializer/serializer_tests.cpp
    io/gro_parser_tests.cpp
    engine/camera/camera_tests.cpp
    engine/cartoon/cartoon_tests.cpp
    engine/export/exporter_tests.cpp
    engine/export/png_encoder_tests.cpp
    engine/quicksurf/quicksurf_tests.cpp
    engine/raytrace/ray_tracer_tests.cpp
    engine/rep/render_mode_tests.cpp
    engine/render/scene_bounds_tests.cpp
    engine/serialize/scene_bridge_tests.cpp
    engine/scheduler/frame_scheduler_tests.cpp
    engine/surface/surface_tests.cpp
    entity/scene/system_tests.cpp
    entity/trajectory/dcd_reader_tests.cpp
    vulkan/line_smoke.cpp
    vulkan/mesh_smoke.cpp
    vulkan/offscreen_smoke.cpp
    vulkan/single_atom_smoke.cpp
    view/layout_serialize_tests.cpp
)
target_link_libraries(moleculens_tests PRIVATE moleculens_engine Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(moleculens_tests)
