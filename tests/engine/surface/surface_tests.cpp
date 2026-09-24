// =============================================================================
// MolecuLens · tests · engine/surface/surface_tests.cpp —— vdW 表面生成测试
// 校验：单原子 / 双原子表面确实产出 TriMesh 几何（Marching Cubes）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "engine/rep/representer.h"
#include "engine/rep/representation.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

#include "spdlog/spdlog.h"

using namespace moleculens;

namespace {

entity::SceneStructure makeSceneWithO() {
    entity::SceneStructure scene;
    entity::MoleculeStructure ms;
    ms.elements.push_back(entity::ElementType::Oxygen);
    entity::Atom a;
    a.element = entity::ElementType::Oxygen;
    ms.atoms.push_back(a);
    ms.atomNameIds.push_back(0);
    ms.key = entity::computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<entity::Trajectory>();
    entity::Timestep ts;
    ts.positions.emplace_back(0.0f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, entity::Transform{}, std::move(traj));
    return scene;
}

uint32_t triMeshVertexCount(engine::SceneCommandList& out) {
    engine::CommandIterator it(out);
    for (bool has = out.first(it); has; has = out.next(it)) {
        if (it.code() == engine::Cmd::TriMesh && it.payloadSize() >= 8) {
            const auto* hdr =
                reinterpret_cast<const engine::TriMeshHeader*>(it.payload());
            return hdr->vertexCount;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("Surface representation emits a TriMesh", "[engine][surface]") {
    entity::SceneStructure scene = makeSceneWithO();
    const entity::MoleculeInstance& inst = scene.instances()[0];

    engine::Representation rep;
    rep.type = engine::RepresentationType::Surface;
    rep.colorScheme = engine::ColorScheme::ElementCPK;
    std::vector<bool> mask(1, true);
    entity::FrameData fd;
    fd.positions = &scene.coordsSoA().positions;

    engine::SceneCommandList out;
    engine::Representer r;
    r.generate(rep, scene, inst, 0, fd, 0, mask, out);

    const uint32_t verts = triMeshVertexCount(out);
    // 单氧原子 vdW 球 → 等值面应生成几百个顶点
    spdlog::info("[test] surface vertexCount={}", verts);
    REQUIRE(verts > 100);
}

TEST_CASE("Surface on empty structure emits no geometry", "[engine][surface]") {
    entity::SceneStructure scene;
    entity::MoleculeStructure ms;
    ms.key = entity::computeStructureKey(ms);   // 空结构
    const uint32_t si = scene.registerStructure(ms);
    auto traj = std::make_shared<entity::Trajectory>();
    entity::Timestep ts;
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, entity::Transform{}, std::move(traj));

    const entity::MoleculeInstance& inst = scene.instances()[0];
    engine::Representation rep;
    rep.type = engine::RepresentationType::Surface;
    std::vector<bool> mask;
    entity::FrameData fd;
    fd.positions = &scene.coordsSoA().positions;
    engine::SceneCommandList out;
    engine::Representer r;
    r.generate(rep, scene, inst, 0, fd, 0, mask, out);
    REQUIRE(triMeshVertexCount(out) == 0);
}
