// =============================================================================
// MolecuLens · tests · engine/async/surface_async_tests.cpp —— 表面生成异步化
// 规格来源：REMAINING_WORK §1.1「异步 + 渐进生成」（原整体系 QuickSurf 约 197 ms
//           主线程阻塞 → 后台线程生成 + 保留旧画面 + 完成后自动换入）
//
// 覆盖：
//   1) 任务队列本身：后台线程执行、完成通知、同键去重、shutdown 可重入；
//   2) Renderer 行为：异步模式下"表面未就绪"时保留旧命令流（不换入半成品），
//      任务完成后自动重建换入；关闭异步（默认）时立即同步生成（老行为不回退）。
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "engine/async/surface_jobs.h"
#include "engine/cmd/cmd.h"
#include "engine/rep/representation.h"
#include "engine/render/renderer.h"
#include "entity/identity/identity.h"
#include "entity/scene/scene.h"

using namespace moleculens;
using namespace moleculens::entity;

namespace {

// 统计命令流里的 TriMesh 条数（表面以 TriMesh 发射；> 0 表示表面已生成）
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

// 建一个"两个碳原子 + 整体系 QuickSurf 表示"的最小场景。
// 原子坐标由调用方给定（不同测试用不同坐标 → 几何指纹不同，避免命中彼此的表面缓存）。
void buildQuickSurfScene(entity::SceneStructure& scene,
                         std::vector<engine::Representation>& reps, float x0, float x1) {
    const uint32_t sysId = scene.addSystem("A");

    MoleculeStructure ms;
    ms.elements = {ElementType::Carbon, ElementType::Carbon};
    Atom atom;
    atom.element = ElementType::Carbon;
    ms.atoms = {atom, atom};
    ms.atomNameIds = {0, 0};
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts;
    ts.positions.emplace_back(x0, 0.0f, 0.0f);
    ts.positions.emplace_back(x1, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts));
    scene.addTrajectoryInstance(si, Transform{}, std::move(traj), sysId);

    engine::Representation rep;
    rep.name = "quicksurf";
    rep.type = engine::RepresentationType::QuickSurf;
    rep.systemSurface = true;          // G5：整体系一个表面（异步化针对的正是这条路）
    rep.surfGridSpacing = 0.35f;       // 小体系用较粗网格，测试快
    reps.clear();
    reps.push_back(rep);
}

// 建两帧轨迹的最小场景：用于验证"预取缓存让表面跟随轨迹帧"。
void buildTwoFrameQuickSurfScene(entity::SceneStructure& scene,
                                 std::vector<engine::Representation>& reps,
                                 float x0, float x1) {
    const uint32_t sysId = scene.addSystem("A");

    MoleculeStructure ms;
    ms.elements = {ElementType::Carbon, ElementType::Carbon};
    Atom atom;
    atom.element = ElementType::Carbon;
    ms.atoms = {atom, atom};
    ms.atomNameIds = {0, 0};
    ms.key = computeStructureKey(ms);
    const uint32_t si = scene.registerStructure(ms);

    auto traj = std::make_shared<Trajectory>();
    Timestep ts0;
    ts0.positions.emplace_back(x0, 0.0f, 0.0f);
    ts0.positions.emplace_back(x1, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts0));
    Timestep ts1;
    ts1.positions.emplace_back(x0 + 0.8f, 0.0f, 0.0f);
    ts1.positions.emplace_back(x1 + 0.8f, 0.0f, 0.0f);
    traj->frames.push_back(std::move(ts1));
    scene.addTrajectoryInstance(si, Transform{}, std::move(traj), sysId);

    engine::Representation rep;
    rep.name = "quicksurf";
    rep.type = engine::RepresentationType::QuickSurf;
    rep.systemSurface = true;
    rep.surfGridSpacing = 0.35f;
    reps.clear();
    reps.push_back(rep);
}

// 命令流里所有 TriMesh 顶点的质心（用于确认表面确实跟着轨迹平移）。
bool meshCentroid(const engine::SceneCommandList& list, glm::vec3& out, uint32_t& verts) {
    out = glm::vec3(0.0f);
    verts = 0;
    engine::CommandIterator it(list);
    for (bool has = list.first(it); has; has = list.next(it)) {
        if (it.code() != engine::Cmd::TriMesh) {
            continue;
        }
        engine::TriMeshHeader hdr{};
        std::memcpy(&hdr, it.payload(), sizeof(hdr));
        const uint8_t* p = it.payload() + sizeof(hdr);
        for (uint32_t i = 0; i < hdr.vertexCount; ++i) {
            engine::TriMeshVertex v{};
            std::memcpy(&v, p + static_cast<size_t>(i) * sizeof(v), sizeof(v));
            out += glm::vec3(v.pos);
            ++verts;
        }
    }
    return verts > 0;
}

} // namespace

TEST_CASE("surface jobs run on worker thread and report completion", "[engine][async]") {
    engine::async::shutdownSurfaceJobs();   // 清掉其它用例的残留状态

    const auto mainThread = std::this_thread::get_id();
    std::atomic<int> done{0};
    std::atomic<bool> ranOffMain{false};

    REQUIRE(engine::async::submitSurfaceJob(101, [&] {
        ranOffMain.store(std::this_thread::get_id() != mainThread);
        done.fetch_add(1);
    }));
    REQUIRE(engine::async::submitSurfaceJob(102, [&] { done.fetch_add(1); }));
    REQUIRE(engine::async::surfaceJobsBusy());

    engine::async::waitForSurfaceJobs();
    REQUIRE(done.load() == 2);
    REQUIRE(ranOffMain.load());                        // 确实在后台线程
    REQUIRE_FALSE(engine::async::surfaceJobsBusy());
    REQUIRE(engine::async::consumeSurfaceJobCompletion());    // 有完成通知
    REQUIRE_FALSE(engine::async::consumeSurfaceJobCompletion());   // 只报一次

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("same surface job key is not queued twice", "[engine][async]") {
    engine::async::shutdownSurfaceJobs();

    std::atomic<bool> gate{false};
    std::atomic<int> runs{0};
    REQUIRE(engine::async::submitSurfaceJob(201, [&] {
        while (!gate.load()) {
            std::this_thread::yield();   // 占住 key，模拟"生成中"
        }
        runs.fetch_add(1);
    }));
    REQUIRE_FALSE(engine::async::submitSurfaceJob(201, [&] { runs.fetch_add(1); }));
    gate.store(true);
    engine::async::waitForSurfaceJobs();
    REQUIRE(runs.load() == 1);

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("async renderer keeps previous command list while surface generates",
          "[engine][render][async]") {
    entity::SceneStructure scene;
    std::vector<engine::Representation> reps;
    buildQuickSurfScene(scene, reps, /*x0=*/0.0f, /*x1=*/1.35f);

    engine::async::shutdownSurfaceJobs();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.setAsyncSurfaceEnabled(true);
    REQUIRE(renderer.asyncSurfaceEnabled());

    renderer.update(0.0f);
    renderer.prepare();
    // 首次重建：表面在后台生成 → 本轮命令流被丢弃（保持旧命令流 = 空），进入等待
    REQUIRE(renderer.surfaceGenerationPending());
    REQUIRE(countTriMesh(renderer.commandList()) == 0);

    engine::async::waitForSurfaceJobs();
    renderer.prepare();   // 任务完成通知 → 重建时缓存命中 → 换入完整命令流
    REQUIRE_FALSE(renderer.surfaceGenerationPending());
    REQUIRE(countTriMesh(renderer.commandList()) > 0);

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("renderer with async disabled still generates surface synchronously",
          "[engine][render][async]") {
    entity::SceneStructure scene;
    std::vector<engine::Representation> reps;
    buildQuickSurfScene(scene, reps, /*x0=*/10.0f, /*x1=*/11.35f);

    engine::async::shutdownSurfaceJobs();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    REQUIRE_FALSE(renderer.asyncSurfaceEnabled());   // 默认关闭

    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.surfaceGenerationPending());
    REQUIRE(countTriMesh(renderer.commandList()) > 0);   // 同步生成，一次到位

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("frozen system surface survives coordinate replay",
          "[engine][render][async][freeze]") {
    entity::SceneStructure scene;
    std::vector<engine::Representation> reps;
    buildQuickSurfScene(scene, reps, /*x0=*/30.0f, /*x1=*/31.35f);
    engine::Representation ball;
    ball.name = "ballstick";
    ball.type = engine::RepresentationType::BallAndStick;
    reps.push_back(ball);   // 多渲染模式：球棍坐标命令 + 整体系表面 TriMesh

    engine::async::shutdownSurfaceJobs();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countTriMesh(renderer.commandList()) > 0);
    const uint64_t serialFull = renderer.commandSerial();
    const uint64_t meshSerialFull = renderer.commandList().meshSerial();

    // 轨迹帧切：坐标变、拓扑不变。冻结表面后应走坐标重放，且旧 TriMesh 保留。
    std::vector<glm::vec3> world = scene.instances()[0].trajectory->frames[0].positions;
    for (auto& p : world) {
        p.x += 2.0f;
    }
    scene.writeFramePositions(0, world);
    renderer.setSurfaceFreeze(true);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.lastPrepareWasReplay());
    REQUIRE(renderer.commandSerial() > serialFull);
    REQUIRE(renderer.commandList().meshSerial() == meshSerialFull);
    REQUIRE(countTriMesh(renderer.commandList()) > 0);

    // 解冻：下一次 prepare 应全量重建并把表面刷到当前帧
    renderer.setSurfaceFreeze(false);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
    REQUIRE(renderer.commandList().meshSerial() != meshSerialFull);
    REQUIRE(countTriMesh(renderer.commandList()) > 0);

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("frozen surface-only playback keeps command serial unchanged",
          "[engine][render][async][freeze]") {
    entity::SceneStructure scene;
    std::vector<engine::Representation> reps;
    buildQuickSurfScene(scene, reps, /*x0=*/40.0f, /*x1=*/41.35f);

    engine::async::shutdownSurfaceJobs();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(countTriMesh(renderer.commandList()) > 0);
    const uint64_t serialBefore = renderer.commandSerial();

    // 只开整体系表面时，冻结播放期间没有任何需要更新的可见命令：
    // 必须保留 serial，否则后端会每帧重传整张表面网格（14 FPS 的根因）。
    std::vector<glm::vec3> world = scene.instances()[0].trajectory->frames[0].positions;
    for (auto& p : world) {
        p.y += 1.0f;
    }
    scene.writeFramePositions(0, world);
    renderer.setSurfaceFreeze(true);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.lastPrepareWasReplay());
    REQUIRE(renderer.commandSerial() == serialBefore);
    REQUIRE(countTriMesh(renderer.commandList()) > 0);

    // 解冻后仍应重建并换入最新坐标对应的表面
    renderer.setSurfaceFreeze(false);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE_FALSE(renderer.lastPrepareWasReplay());
    REQUIRE(renderer.commandSerial() > serialBefore);

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("prefetched QuickSurf follows trajectory frame",
          "[engine][render][async][prefetch]") {
    entity::SceneStructure scene;
    std::vector<engine::Representation> reps;
    buildTwoFrameQuickSurfScene(scene, reps, /*x0=*/70.0f, /*x1=*/71.35f);

    engine::async::shutdownSurfaceJobs();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 2);
    renderer.setAsyncSurfaceEnabled(true);

    // 第 0 帧：首次异步生成
    scene.applyTrajectoryFrame(0, 0);
    renderer.setTrajectoryFrame(0);
    renderer.update(0.0f);
    renderer.prepare();
    engine::async::waitForSurfaceJobs();
    renderer.update(0.0f);
    renderer.prepare();          // 换入第 0 帧表面，并在命中时提交后续帧预取
    engine::async::waitForSurfaceJobs();

    glm::vec3 c0(0.0f);
    uint32_t v0 = 0;
    REQUIRE(meshCentroid(renderer.commandList(), c0, v0));
    c0 /= static_cast<float>(v0);
    const uint64_t meshSerial0 = renderer.commandList().meshSerial();

    // 第 1 帧：应直接命中预取缓存，表面整体 +0.8 Å
    scene.applyTrajectoryFrame(0, 1);
    renderer.setTrajectoryFrame(1);
    renderer.update(0.0f);
    renderer.prepare();

    glm::vec3 c1(0.0f);
    uint32_t v1 = 0;
    REQUIRE(meshCentroid(renderer.commandList(), c1, v1));
    c1 /= static_cast<float>(v1);
    REQUIRE(c1.x > c0.x + 0.3f);            // 表面确实换到了新帧
    REQUIRE(v1 > 0);
    REQUIRE(renderer.commandList().meshSerial() != meshSerial0);

    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("prefetch jobs do not block current frame rebuild",
          "[engine][async][prefetch]") {
    engine::async::shutdownSurfaceJobs();
    const uint64_t submittedBefore = engine::async::submittedSurfaceJobCount();
    REQUIRE(engine::async::submitPrefetchSurfaceJob(0xFEED1234ull, [] {}));
    // 预取只写缓存，不应让 Renderer 误判"当前帧命令流不完整"
    REQUIRE_FALSE(engine::async::surfaceJobsBusy());
    REQUIRE(engine::async::submittedSurfaceJobCount() == submittedBefore);
    engine::async::waitForSurfaceJobs();
    engine::async::shutdownSurfaceJobs();
}

TEST_CASE("prepareBlocking resolves pending surface before export", "[engine][render][async]") {
    entity::SceneStructure scene;
    std::vector<engine::Representation> reps;
    buildQuickSurfScene(scene, reps, /*x0=*/20.0f, /*x1=*/21.35f);

    engine::async::shutdownSurfaceJobs();
    engine::Renderer renderer;
    renderer.setScene(&scene);
    renderer.bindRepresentations(&reps, 1);
    renderer.setAsyncSurfaceEnabled(true);
    renderer.update(0.0f);
    renderer.prepare();
    REQUIRE(renderer.surfaceGenerationPending());

    // 导出路径：阻塞等待 + 同步重建，返回时命令流必须已含表面
    renderer.prepareBlocking();
    REQUIRE_FALSE(renderer.surfaceGenerationPending());
    REQUIRE(countTriMesh(renderer.commandList()) > 0);

    engine::async::shutdownSurfaceJobs();
}
