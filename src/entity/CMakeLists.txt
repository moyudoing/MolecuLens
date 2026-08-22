# =============================================================================
# MolecuLens · src/entity —— 数据层（实体 + IO + 持久化）
# 规格来源：M0 工程脚手架 §4.1（io/persist 为兄弟目录，经 ../ 纳入本目标）
# =============================================================================

add_library(moleculens_entity STATIC
    element/element.h element/element.cpp
    atom/atom.h
    molecule/molecule.h
    identity/identity.h identity/identity.cpp
    scene/scene.h scene/scene.cpp scene/transform.h
    trajectory/timestep.h trajectory/trajectory.h trajectory/trajectory_reader.h
    selection/selection.h selection/selection.cpp selection/parse_table.h
    search/bond_search.h search/bond_search.cpp
    ../io/registry/ifile_reader.h ../io/registry/parser_registry.h ../io/registry/parser_registry.cpp
    ../io/pdb/pdb_parser.h ../io/pdb/pdb_parser.cpp
    ../io/gro/gro_parser.h ../io/gro/gro_parser.cpp
    ../persist/mlscene/mlscene.h ../persist/mlscene/scene_serializer.h ../persist/mlscene/scene_serializer.cpp
    ../persist/settings/settings.h ../persist/settings/settings.cpp
    ../persist/cache/cache.h ../persist/cache/cache.cpp
)
target_include_directories(moleculens_entity PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(moleculens_entity PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    glm::glm)
