// =============================================================================
// MolecuLens · tests · engine/surface/surface_tests.cpp —— vdW 表面生成测试
// 校验：单原子 / 双原子表面确实产出 TriMesh 几何（Marching Cubes）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/rep/representer.h"
#include "engine/rep/representation.h"
#include "engine/render/renderer.h"
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

// 2 实例各 1 原子（C / O，相距 3 Å）：整体系表面必须同时覆盖两个实例，且顶点色
// 按各自元素取（灰 / 红）——同时暴露"空 ctx.instance 崩溃"和"整体系表面只有单色"。
entity::SceneStructure makeSceneWithTwoElements() {
    entity::SceneStructure scene;
    const auto addOne = [&scene](entity::ElementType el, const glm::vec3& pos,
                                 const char* name) {
        entity::MoleculeStructure ms;
        ms.elements.push_back(el);
        entity::Atom a;
        a.element = el;
        ms.atoms.push_back(a);
        ms.atomNameIds.push_back(scene.strings().intern(name));
        ms.key = entity::computeStructureKey(ms);
        const uint32_t si = scene.registerStructure(ms);
        auto traj = std::make_shared<entity::Trajectory>();
        entity::Timestep ts;
        ts.positions.push_back(pos);
        traj->frames.push_back(std::move(ts));
        scene.addTrajectoryInstance(si, entity::Transform{}, std::move(traj));
    };
    addOne(entity::ElementType::Carbon, glm::vec3(0.0f), "C");
    addOne(entity::ElementType::Oxygen, glm::vec3(3.0f, 0.0f, 0.0f), "O");
    return scene;
}

uint32_t countTriMesh(const engine::SceneCommandList& list) {
    uint32_t n = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::TriMesh) {
            ++n;
        }
    }
    return n;
}

// 收集全部 TriMesh 顶点色（appendTriMesh 会切块，所以必须遍历所有块）
std::vector<glm::vec4> meshVertexColors(const engine::SceneCommandList& list) {
    std::vector<glm::vec4> colors;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() != engine::Cmd::TriMesh ||
            it.payloadSize() < sizeof(engine::TriMeshHeader)) {
            continue;
        }
        engine::TriMeshHeader hdr{};
        std::memcpy(&hdr, it.payload(), sizeof(hdr));
        const uint8_t* raw = it.payload() + sizeof(engine::TriMeshHeader);
        for (uint32_t i = 0; i < hdr.vertexCount; ++i) {
            engine::TriMeshVertex v;
            std::memcpy(&v,
                        raw + static_cast<size_t>(i) * sizeof(engine::TriMeshVertex),
                        sizeof(v));
            colors.push_back(v.color);
        }
    }
    return colors;
}

bool hasDistinctColors(const std::vector<glm::vec4>& colors) {
    if (colors.size() < 2) {
        return false;
    }
    const glm::vec4& c0 = colors.front();
    for (const auto& c : colors) {
        if (std::fabs(c.r - c0.r) > 1e-3f || std::fabs(c.g - c0.g) > 1e-3f ||
            std::fabs(c.b - c0.b) > 1e-3f) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> renderModes(const engine::SceneCommandList& list) {
    std::vector<uint8_t> modes;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() == engine::Cmd::RenderMode &&
            it.payloadSize() >= sizeof(engine::RenderModeInst)) {
            engine::RenderModeInst rm{};
            std::memcpy(&rm, it.payload(), sizeof(rm));
            modes.push_back(rm.mode);
        }
    }
    return modes;
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

// =============================================================================
// v2.19 回归：整体系表面（systemSurface）在**没有 ctx.instance** 的上下文里生成。
// 历史缺陷：drawSystemSurface 对每个原子调用 colorForAtom(ctx, g)，后者读
// ctx.instance->structureIndex → 空指针解引用，CLI --system-surface 必崩
// （0xC0000005）；逐分子的 --rep surface 则正常。
// 本用例走 Renderer::prepare 的真实整体系分支（无 GPU 用法，同
// render_mode_tests.cpp），同时断言表面覆盖 2 个实例且逐顶点按元素着色
// （修复前后半段是"整体系表面只用原始元素色、忽略 colorScheme"的另一处缺陷）。
// =============================================================================
TEST_CASE("system surface builds across instances and colours by element",
          "[engine][surface]") {
    entity::SceneStructure scene = makeSceneWithTwoElements();
    REQUIRE(scene.instances().size() == 2);

    std::vector<engine::Representation> reps(1);
    reps[0].name = "system-surface";
    reps[0].type = engine::RepresentationType::Surface;
    reps[0].systemSurface = true;
    reps[0].surfGridSpacing = 0.4f;

    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();   // 修复前：这里崩溃（空 ctx.instance）
    const auto& list = renderer.commandList();
    REQUIRE(countTriMesh(list) >= 1);

    const std::vector<glm::vec4> colors = meshVertexColors(list);
    spdlog::info("[test] system surface vertexCount={}", colors.size());
    REQUIRE(colors.size() > 100);
    REQUIRE(hasDistinctColors(colors));   // 碳灰 vs 氧红：不是全同色
}

// =============================================================================
// v2.19：表面表示自己的「绘制模式」必须真的改变发射的 RenderMode（此前整体系
// 表面被渲染器强制 mode 0，用户根本没有可用的线框/半透明表面）。
// 这里验证逐分子路径（Representer::generate）：0 = 跟随体系模式（旧行为）、
// 1 = 线框、2 = 半透明。
// =============================================================================
TEST_CASE("surfaceDrawMode overrides the emitted RenderMode", "[engine][surface]") {
    entity::SceneStructure scene = makeSceneWithO();
    const entity::MoleculeInstance& inst = scene.instances()[0];
    entity::FrameData fd;
    fd.positions = &scene.coordsSoA().positions;
    std::vector<bool> mask(1, true);

    const auto modesFor = [&](uint8_t drawMode) {
        engine::Representation rep;
        rep.type = engine::RepresentationType::Surface;
        rep.surfaceDrawMode = drawMode;
        engine::SceneCommandList out;
        engine::Representer r;
        r.generate(rep, scene, inst, 0, fd, 0, mask, out);
        return renderModes(out);
    };

    const std::vector<uint8_t> solid = modesFor(0);
    REQUIRE(solid.size() == 1);
    REQUIRE(solid[0] == 0u);

    const std::vector<uint8_t> wire = modesFor(1);
    REQUIRE(wire.size() == 1);
    REQUIRE(wire[0] == 1u);   // 线框

    const std::vector<uint8_t> transparent = modesFor(2);
    REQUIRE(transparent.size() == 1);
    REQUIRE(transparent[0] == 2u);   // 半透明
}
