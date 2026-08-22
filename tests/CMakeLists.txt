# =============================================================================
# MolecuLens · tests —— 单元 / 集成测试
# 规格来源：M0 工程脚手架 §4.5 / 主方案 §14.3
# =============================================================================

add_executable(moleculens_tests
    entity/element/element_tests.cpp
    entity/identity/identity_tests.cpp
    entity/selection/selection_tests.cpp
    entity/serializer/serializer_tests.cpp
    engine/camera/camera_tests.cpp
    engine/cartoon/cartoon_tests.cpp
    engine/scheduler/frame_scheduler_tests.cpp
    vulkan/offscreen_smoke.cpp
    view/layout_serialize_tests.cpp
)
target_link_libraries(moleculens_tests PRIVATE moleculens_engine Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(moleculens_tests)
