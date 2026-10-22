# =============================================================================
# MolecuLens · shaders —— 着色器编译内嵌
# 规格来源：M0 工程脚手架 §4.8（triangle 清屏测试；其余编译后内嵌）
# 说明：着色器头文件由 engine（realtime/offscreen）引用，统一挂到
#       moleculens_engine 目标（app/cli 经 engine 传递获得生成头）。
# =============================================================================

include(${CMAKE_SOURCE_DIR}/cmake/shaders.cmake)

# 共享 GLSL 头（经 #include 引入）必须纳入 configure 依赖：
# 着色器在 **configure 期**编译成 .spv + 内嵌头，而 ${src} 只是各 .vert/.frag ——
# 改了被 include 的 .glsl 不会触发重新 configure → 内嵌 SPIR-V 陈旧 →
# 着色器里的 UBO 布局与 C++ 悄悄不一致（"看着没错但渲染结果不对"）。
# 这与"陈旧 .obj 导致类布局错位"是同一类坑，故显式登记依赖。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/global_ubo.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/material_shading.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/rt_common.glsl)

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
# 点模式（体系渲染模式 = 点）
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/point.vert
                      ${CMAKE_BINARY_DIR}/gen/shaders/point_vert.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/point.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/point_frag.h)
# 方向光阴影的深度 pass（片元阶段无输出；顶点阶段复用 sphere/cylinder/mesh.vert）
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/shadow.frag
                      ${CMAKE_BINARY_DIR}/gen/shaders/shadow_frag.h)
# GPU 光追（任务 E）：raygen / miss / shadow miss / triangle closest-hit /
# 程序化求交（球+胶囊 AABB）；--target-env vulkan1.2 由 cmake/shaders.cmake
# 按扩展名自动附加，栅格化着色器命令不变。
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/rt_raygen.rgen
                      ${CMAKE_BINARY_DIR}/gen/shaders/rt_raygen_rgen.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/rt_miss.rmiss
                      ${CMAKE_BINARY_DIR}/gen/shaders/rt_miss_rmiss.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/rt_shadow.rmiss
                      ${CMAKE_BINARY_DIR}/gen/shaders/rt_shadow_rmiss.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/rt_tri.rchit
                      ${CMAKE_BINARY_DIR}/gen/shaders/rt_tri_rchit.h)
moleculens_add_shader(moleculens_engine ${CMAKE_CURRENT_SOURCE_DIR}/rt_prim.rint
                      ${CMAKE_BINARY_DIR}/gen/shaders/rt_prim_rint.h)
