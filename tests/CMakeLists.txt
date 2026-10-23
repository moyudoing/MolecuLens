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
    io/pdb_parser_tests.cpp
    io/cube_tests.cpp
    engine/camera/camera_tests.cpp
    engine/async/surface_async_tests.cpp
    engine/cartoon/cartoon_tests.cpp
    engine/export/exporter_tests.cpp
    engine/export/png_encoder_tests.cpp
    engine/export/esp_overlay_tests.cpp
    engine/quicksurf/quicksurf_tests.cpp
    engine/espsurface/esp_surface_tests.cpp
    engine/raytrace/ray_tracer_tests.cpp
    engine/rep/render_mode_tests.cpp
    engine/rep/instance_range_tests.cpp
    engine/render/coord_replay_tests.cpp
    engine/render/pane_filter_tests.cpp
    engine/render/scene_bounds_tests.cpp
    engine/serialize/scene_bridge_tests.cpp
    engine/scheduler/frame_scheduler_tests.cpp
    engine/surface/surface_tests.cpp
    entity/scene/system_tests.cpp
    entity/trajectory/dcd_reader_tests.cpp
    entity/trajectory/multiframe_tests.cpp
    entity/trajectory/xtc_reader_tests.cpp
    entity/trajectory/trr/trr_reader_tests.cpp
    vulkan/line_smoke.cpp
    vulkan/mesh_smoke.cpp
    vulkan/offscreen_smoke.cpp
    vulkan/rt_smoke.cpp
    vulkan/single_atom_smoke.cpp
    view/layout_serialize_tests.cpp
    # 分析统计 / XLSX 导出是 app 层纯数据代码（不依赖 ImGui/Vulkan），直接编进
    # 测试目标，避免为它们再造一个库。
    ${CMAKE_SOURCE_DIR}/src/app/workbench/analysis/molecule_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/app/workbench/analysis/xlsx_writer.cpp
    app/analysis_tests.cpp
)
target_link_libraries(moleculens_tests PRIVATE moleculens_engine Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(moleculens_tests)

# =============================================================================
# 渲染性能基准（独立可执行文件，**不进** ctest）
# 规格来源：性能测试需求（md.gro + md.xtc；加载/首帧/FPS/内存/显存/交互延迟）
# =============================================================================
# 为什么不放进 moleculens_tests：
#   · 性能测试依赖机器状态（GPU/驱动/后台负载），进 CI 会随机变红；
#   · 需要 CLI 参数（--frames/--rep/分辨率），Catch2 用例不便传参；
#   · 它只报告度量、不判定阈值 —— 判定交给 tools/run_bench.ps1 与人工分析。
add_executable(moleculens_bench bench/bench_main.cpp)
target_link_libraries(moleculens_bench PRIVATE moleculens_engine)
