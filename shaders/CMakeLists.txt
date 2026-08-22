# =============================================================================
# MolecuLens · shaders —— 着色器编译内嵌
# 规格来源：M0 工程脚手架 §4.8（triangle 清屏测试；其余编译后内嵌）
# 说明：着色器头文件由 engine（realtime/offscreen）引用，统一挂到
#       moleculens_engine 目标（app/cli 经 engine 传递获得生成头）。
# =============================================================================

include(${CMAKE_SOURCE_DIR}/cmake/shaders.cmake)

# 清屏测试着色器（M0）
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/triangle.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/triangle_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/triangle.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/triangle_frag.h)
# 分子渲染着色器（M1）
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/sphere.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/sphere_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/sphere.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/sphere_frag.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/cylinder.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/cylinder_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/cylinder.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/cylinder_frag.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/line.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/line_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/line.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/line_frag.h)
# 三角网格（卡通/表面，TriMeshVertex 48B）
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/mesh.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/mesh_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/mesh.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/mesh_frag.h)
# 拾取 ID Pass（M2）
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/pick.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/pick_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/pick_cyl.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/pick_cyl_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/pick.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/pick_frag.h)
