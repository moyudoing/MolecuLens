// =============================================================================
// MolecuLens · tests/bench/bench_main.cpp —— 渲染性能基准（headless GPU）
//
// 目的：在**可复现、无人工介入**的前提下量化分子可视化渲染模块的性能，并把
//       耗时拆到各阶段，使瓶颈能被定位而不是只看到一个总数。
//
// 为什么是独立可执行文件而不是 Catch2 用例：
//   性能测试依赖机器状态（GPU/驱动/后台负载），放进单元测试套件会让 CI 结果
//   随机变红；且需要 `--frames`/分辨率/表示等参数。本程序退出码 0 = 跑完，
//   不判定阈值 —— 判定交给汇总脚本与人工分析。
//
// 测量口径（关键，否则数字会互相矛盾）：
//   · **交互帧**（相机旋转/缩放/拖拽）= 只重算 viewProj 并重新提交，不重建命令流
//     —— 这是用户在视口里拖动时的真实路径。
//   · **轨迹帧切** = applyTrajectoryFrame（写坐标段）+ prepare（坐标重放/全量重建）
//     + 提交 —— 播放轨迹时的真实路径。
//   · **不含回读**：GUI 视口把离屏纹理直接交给 ImGui 采样，不回读 CPU。
//     回读（readbackPixels）只用于导出，单独测（--readback）以免污染 FPS。
//   · execute() 内部以单次命令缓冲提交并等待完成 → 计时包含 GPU 执行时间。
//
// 用法：
//   moleculens_bench [--gro P] [--xtc P] [--frames N] [--w W] [--h H]
//                    [--rep NAME] [--hide-solvent] [--readback] [--rebuild-every N]
//                    [--system-surface] [--async-surface]
//   --rep: ballstick|stick|vdw|spacefill|licorice|cartoon|tube|ribbon|newcartoon|
//          surface|quicksurf（后两者配 --system-surface = 整体系一个表面）
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/export/png/png_encoder.h"   // writePng（首帧截图核查）
#include "engine/render/render_parameter.h"  // 材质库（--material）
#include "engine/render/renderer.h"
#include "engine/rep/representation.h"
#include "engine/vulkan/backend/decode.h"   // commandStats
#include "engine/vulkan/device/device.h"    // queryVramUsage
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/element/element.h"
#include "entity/scene/scene.h"
#include "entity/trajectory/xtc/xtc_reader.h"
#include "io/registry/parser_registry.h"

using namespace moleculens;

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

double msSince(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count() * 1000.0;
}

// 帧时间统计（保留全样本以便取分位数；轨迹长度 ≤ 数千，内存可忽略）
struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    [[nodiscard]] double sum() const {
        double s = 0.0;
        for (double x : v) s += x;
        return s;
    }
    [[nodiscard]] double avg() const { return v.empty() ? 0.0 : sum() / v.size(); }
    [[nodiscard]] double pct(double p) const {
        if (v.empty()) return 0.0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        const size_t i = static_cast<size_t>(p * (s.size() - 1) + 0.5);
        return s[std::min(i, s.size() - 1)];
    }
    [[nodiscard]] double max() const {
        return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
    }
};

// 进程 RSS（工作集）：peak = 峰值工作集，cur = 当前
struct RssInfo {
    uint64_t peakBytes = 0;
    uint64_t curBytes = 0;
    bool ok = false;
};

RssInfo readRss() {
    RssInfo r;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        r.peakBytes = pmc.PeakWorkingSetSize;
        r.curBytes = pmc.WorkingSetSize;
        r.ok = true;
    }
#endif
    return r;
}

// 进程 CPU 时间（内核 + 用户），用于算"CPU 占用率 = CPU 时间 / 墙钟时间"
double readCpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        auto toSec = [](const FILETIME& f) {
            ULARGE_INTEGER x;
            x.LowPart = f.dwLowDateTime;
            x.HighPart = f.dwHighDateTime;
            return static_cast<double>(x.QuadPart) * 1e-7;   // 100ns 单位
        };
        return toSec(k) + toSec(u);
    }
    return 0.0;
#else
    return 0.0;
#endif
}

double mib(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

engine::RepresentationType parseRep(const std::string& s) {
    using engine::RepresentationType;
    if (s == "wireframe") return RepresentationType::Wireframe;
    if (s == "stick") return RepresentationType::Stick;
    if (s == "spacefill" || s == "cpk") return RepresentationType::Spacefill;
    if (s == "vdw") return RepresentationType::Vdw;
    if (s == "licorice") return RepresentationType::Licorice;
    if (s == "cartoon") return RepresentationType::Cartoon;
    if (s == "tube") return RepresentationType::Tube;
    if (s == "ribbon") return RepresentationType::Ribbon;
    if (s == "newcartoon") return RepresentationType::NewCartoon;
    if (s == "surface") return RepresentationType::Surface;
    if (s == "quicksurf") return RepresentationType::QuickSurf;
    return RepresentationType::BallAndStick;
}

// 一档测量：跑 n 帧，每帧由 fn 决定"这一帧做什么"
template <typename Fn>
Stats measure(int n, Fn&& fn) {
    Stats st;
    st.v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        fn(i);
        st.add(msSince(t0));
    }
    return st;
}

void reportLine(const char* label, const Stats& s) {
    if (s.v.empty()) return;
    const double a = s.avg();
    std::printf("  %-22s avg %7.2f ms  p50 %7.2f  p95 %7.2f  max %7.2f  |  "
                "FPS avg %6.1f  min %6.1f\n",
                label, a, s.pct(0.50), s.pct(0.95), s.max(),
                a > 0.0 ? 1000.0 / a : 0.0,
                s.max() > 0.0 ? 1000.0 / s.max() : 0.0);
}

} // namespace

int main(int argc, char** argv) {
    std::string groPath = "F:/MolecuLens/data/md.gro";
    std::string xtcPath = "F:/MolecuLens/data/md.xtc";
    std::string repName = "ballstick";
    int frames = 0;             // 0 = 全部
    uint32_t w = 1600, h = 900;
    int steadyFrames = 0;       // 0 = 自动（= 轨迹帧数，上限 300）
    bool hideSolvent = false;
    bool doReadback = false;    // 额外测一次"含回读"的导出成本
    int rebuildEvery = 0;       // >0 = 每 N 帧强制一次全量重建（对照重放）
    bool parseOnly = false;     // 只测结构解析（规模扫掠用；不要求轨迹原子数匹配）
    std::string pngPath;        // 非空 = 首帧额外写出 PNG（供肉眼核查渲染效果）
    std::string materialName;   // 材质预设名（kMaterialPresets，默认空 = Opaque）
    bool verbose = false;       // 额外诊断（超长圆柱等渲染异常信号）
    bool systemSurface = false; // 体系级合并表面（Surface/QuickSurf：整体系一个面）
    bool asyncSurface = false;  // 异步表面生成（量化"主线程不阻塞"；见 [G] 档）

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--gro") groPath = next();
        else if (a == "--xtc") xtcPath = next();
        else if (a == "--frames") frames = std::atoi(next().c_str());
        else if (a == "--w") w = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "--h") h = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "--rep") repName = next();
        else if (a == "--steady") steadyFrames = std::atoi(next().c_str());
        else if (a == "--hide-solvent") hideSolvent = true;
        else if (a == "--readback") doReadback = true;
        else if (a == "--rebuild-every") rebuildEvery = std::atoi(next().c_str());
        else if (a == "--parse-only") parseOnly = true;
        else if (a == "--png") pngPath = next();
        else if (a == "--material") materialName = next();
        else if (a == "--verbose") verbose = true;
        else if (a == "--system-surface") systemSurface = true;
        else if (a == "--async-surface") asyncSurface = true;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    const double cpuBegin = readCpuSeconds();
    const auto wallBegin = Clock::now();

    std::printf("=== MolecuLens 渲染性能基准 ===\n");
    std::printf("结构 %s\n轨迹 %s\n表示 %s | 分辨率 %ux%u | 溶剂%s | 帧数上限 %d\n\n",
                groPath.c_str(), xtcPath.c_str(), repName.c_str(), w, h,
                hideSolvent ? "隐藏" : "显示", frames);

    // —— ① 加载阶段 ——
    std::printf("[1] 加载\n");
    auto t0 = Clock::now();
    auto parsed = entity::ParserRegistry::defaultRegistry().parseFile(groPath, {});
    const double tParseGro = msSince(t0);
    if (!parsed.has_value() || parsed->scene == nullptr) {
        std::fprintf(stderr, "解析结构失败: %s\n", parsed.errorMessage().c_str());
        return 1;
    }
    std::shared_ptr<entity::SceneStructure> scene = parsed->scene;

    size_t totalAtoms = 0, solventInstances = 0;
    for (const auto& in : scene->instances()) {
        totalAtoms += scene->structures()[in.structureIndex].elements.size();
        if (in.isSolvent) ++solventInstances;
    }
    std::printf("  %-22s %7.2f ms   (%zu 实例 / %zu 原子 / %zu 结构, 溶剂实例 %zu)\n",
                "解析 GRO", tParseGro, scene->instances().size(), totalAtoms,
                scene->structures().size(), solventInstances);

    if (parseOnly) {
        // 规模扫掠模式：轨迹与渲染需要原子数匹配/GPU，这里只报告解析耗时
        std::printf("\nBENCH_JSON {\"parseOnly\":true,\"atoms\":%zu,\"instances\":%zu,"
                    "\"structures\":%zu,\"parseGroMs\":%.2f}\n",
                    totalAtoms, scene->instances().size(), scene->structures().size(),
                    tParseGro);
        return 0;
    }

    if (hideSolvent) {
        scene->setSolventVisible(false);
    }

    // 轨迹：读取 → 挂载（体系级：md.gro 是多实例体系）
    t0 = Clock::now();
    entity::XtcReader reader;
    auto opened = reader.open(xtcPath);
    const double tOpenXtc = msSince(t0);
    if (!opened.has_value()) {
        std::fprintf(stderr, "打开 XTC 失败: %s\n", opened.errorMessage().c_str());
        return 1;
    }
    const uint64_t nFrames = (frames > 0)
                                 ? std::min<uint64_t>(frames, reader.frameCount())
                                 : reader.frameCount();
    std::printf("  %-22s %7.2f ms   (%u 原子 × %llu/%llu 帧, dt=%g ps)\n", "打开 XTC",
                tOpenXtc, reader.atomCount(), static_cast<unsigned long long>(nFrames),
                static_cast<unsigned long long>(reader.frameCount()), reader.dt());

    t0 = Clock::now();
    auto traj = std::make_shared<entity::Trajectory>();
    traj->dt = reader.dt();
    traj->frames.reserve(static_cast<size_t>(nFrames));
    for (uint64_t f = 0; f < nFrames; ++f) {
        auto fr = reader.readFrame(f);
        if (!fr) break;
        traj->frames.push_back(std::move(fr.value()));
    }
    const double tReadFrames = msSince(t0);
    const double trajMiB = static_cast<double>(nFrames) * reader.atomCount() * 12.0 /
                           (1024.0 * 1024.0);
    std::printf("  %-22s %7.2f ms   (%.1f MB 坐标, %.0f MB/s)\n", "读入全部帧",
                tReadFrames, trajMiB,
                tReadFrames > 0.0 ? trajMiB / (tReadFrames / 1000.0) : 0.0);

    if (traj->frames.empty()) {
        std::fprintf(stderr, "轨迹不含有效帧\n");
        return 1;
    }

    const uint32_t sysId = scene->systems().empty() ? UINT32_MAX
                                                    : scene->systems()[0].id;
    t0 = Clock::now();
    const bool attached = scene->setSystemTrajectory(sysId, traj);
    const double tAttach = msSince(t0);
    if (!attached) {
        std::fprintf(stderr, "体系级轨迹挂载失败（原子数不符？）\n");
        return 1;
    }
    std::printf("  %-22s %7.2f ms   (体系 id=%u)\n", "挂载体系轨迹", tAttach, sysId);

    // —— ② 命令流构建 + 首帧 ——
    std::printf("\n[2] 首帧\n");
    std::vector<engine::Representation> reps(1);
    reps[0].name = repName;
    reps[0].type = parseRep(repName);
    reps[0].systemSurface = systemSurface;
    if (!materialName.empty()) {
        // 材质预设：走与 UI 完全相同的取值路径（materialPresetAt 单一事实来源）
        const int mi = engine::materialPresetIndexByName(materialName.c_str());
        if (mi < 0) {
            std::fprintf(stderr, "未知材质名: %s\n", materialName.c_str());
            return 2;
        }
        const engine::MaterialPreset& p = engine::materialPresetAt(mi);
        reps[0].materialPreset = mi;
        for (int c = 0; c < 3; ++c) {
            reps[0].ambient[c] = p.ambient[c];
            reps[0].diffuse[c] = p.diffuse[c];
            reps[0].specular[c] = p.specular[c];
        }
        // alpha 分量恒为 1：透明度由 opacity 字段承载（shader 用 matOpacity × vColor.a）
        reps[0].ambient[3] = reps[0].diffuse[3] = reps[0].specular[3] = 1.0f;
        reps[0].shininess = p.shininess;
        reps[0].metallic = p.metallic;
        reps[0].roughness = p.roughness;
        reps[0].opacity = p.opacity;
    }

    engine::Renderer renderer;
    renderer.setScene(scene.get());
    renderer.bindRepresentations(&reps, 1);

    t0 = Clock::now();
    renderer.update(0.0f);
    renderer.prepare();
    const double tBuildCmd = msSince(t0);
    engine::CommandStats cs{};
    engine::commandStats(renderer.commandList(), &cs);
    // 注意 size() 是 uint32_t：用 %zu 打印会因可变参数类型不匹配读出垃圾值
    std::printf("  %-22s %7.2f ms   (%u 球 / %u 柱 / %u 线 / %u 网格, %u 字节命令流)\n",
                "构建命令流", tBuildCmd, cs.spheres, cs.cylinders, cs.lines, cs.meshes,
                renderer.commandList().size());

    // 诊断：命令流里"明显过长"的圆柱 = 两端原子相距过远的键。
    // 分子内键长 ≤ ~1.6 Å，跨分子的键是渲染缺陷的可靠信号（几何被错位/错配），
    // 而它在画面上只表现为几根莫名的长棍，肉眼很难归因到具体实例。
    if (verbose) {
        // 批处理诊断：材质槽数与各几何数组的分段数。段数 = 该类图元的绘制调用数，
        // 与图元数一起看即可判断"每次绘制摊到多少实例"（理想 = 每类 1 段）。
        engine::DrawList dl;
        engine::decodeCommandList(renderer.commandList(), dl);
        size_t pointCyl = 0;
        for (uint8_t m : dl.cylinderMode) {
            if (m == static_cast<uint8_t>(engine::DrawMode::Point)) ++pointCyl;
        }
        const auto sr = engine::computeDrawRuns(dl.sphereMat, dl.sphereMode, dl.spheres.size());
        const auto cr =
            engine::computeDrawRuns(dl.cylinderMat, dl.cylinderMode, dl.cylinders.size());
        std::printf("  [诊断] 材质槽 %zu 个 | 球 %zu 段 | 柱 %zu 段（其中点模式 %zu 个 → 线段绘制）"
                    " | 线 %zu | 网格 %zu\n",
                    dl.materials.size(), sr.size(), cr.size(), pointCyl, dl.lines.size(),
                    dl.meshes.size());
        std::printf("  [诊断] 超长圆柱（> 10 A）:\n");
        engine::CommandIterator it(renderer.commandList());
        uint32_t bad = 0;
        for (bool has = renderer.commandList().first(it); has;
             has = renderer.commandList().next(it)) {
            if (it.code() != engine::Cmd::CylinderInst ||
                it.payloadSize() < sizeof(engine::CylinderInst)) {
                continue;
            }
            const auto* cyl = reinterpret_cast<const engine::CylinderInst*>(it.payload());
            const float len = glm::length(glm::vec3(cyl->bPos) - glm::vec3(cyl->aPos));
            if (len > 10.0f) {
                if (bad < 12) {
                    std::printf("     len=%7.2f A  pickId=%u  a=(%.1f,%.1f,%.1f) "
                                "b=(%.1f,%.1f,%.1f)\n",
                                len, cyl->pickId_pad.x, cyl->aPos.x, cyl->aPos.y,
                                cyl->aPos.z, cyl->bPos.x, cyl->bPos.y, cyl->bPos.z);
                }
                ++bad;
            }
        }
        std::printf("     共 %u 根 / %u 根圆柱\n", bad, cs.cylinders);
    }

    engine::OrbitCamera camera;
    glm::vec3 center(0.0f);
    float radius = 10.0f;
    if (!renderer.sceneBounds(&center, &radius)) {
        std::fprintf(stderr, "场景包围球不可用\n");
        return 1;
    }
    camera.centerOn(center, radius);

    engine::RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;   // 离屏附件恒 1x（与导出/FBO 硬约束一致）
    engine::OffscreenRenderer off;
    std::string err;
    if (!off.init(cfg, w, h, &err)) {
        std::fprintf(stderr, "离屏渲染器初始化失败（无 GPU？）: %s\n", err.c_str());
        return 1;
    }

    t0 = Clock::now();
    off.execute(renderer.commandList(), renderer.viewFor(camera, w, h));
    std::vector<uint8_t> px;
    const bool firstReadback = off.readbackPixels(px, engine::PixelFormat::Rgba8);
    const double tFirstFrame = msSince(t0);
    std::printf("  %-22s %7.2f ms   (含回读, %s)\n", "首帧 渲染+回读", tFirstFrame,
                firstReadback ? "ok" : "失败");
    // 绘制调用数 = 批处理是否生效的直接证据（见 engine/vulkan/backend/draw_list.h
    // 的 computeDrawRuns）。每 (表示, 模式) 一次是理想值；接近图元数说明段被切断。
    const uint32_t drawCalls = off.lastDrawCalls();
    std::printf("  %-22s %u 次   (%u 球 + %u 柱, 平均每次 %.1f 个实例)\n", "绘制调用",
                drawCalls, cs.spheres, cs.cylinders,
                drawCalls ? static_cast<double>(cs.spheres + cs.cylinders) / drawCalls
                          : 0.0);
    if (!pngPath.empty() && firstReadback) {
        // 写出首帧供肉眼核查（光照/材质/抗锯齿/配色/背景）。离屏清屏 alpha=1，
        // 几何按材质 + 半球光 + 方向光着色 —— 与 GUI 视口同一条管线。
        if (engine::writePng(pngPath, w, h, px)) {
            std::printf("  %-22s %s\n", "首帧 PNG", pngPath.c_str());
        } else {
            std::printf("  %-22s 写出失败: %s\n", "首帧 PNG", pngPath.c_str());
        }
    }

    const RssInfo rssAfterLoad = readRss();
    engine::VulkanDevice::VramUsage vram{};
    const bool haveVram = off.device().queryVramUsage(&vram);

    // —— ③ 稳态三档 ——
    const int N = (steadyFrames > 0)
                      ? steadyFrames
                      : static_cast<int>(std::min<uint64_t>(nFrames, 300));
    std::printf("\n[3] 稳态 (%d 帧/档)\n", N);

    // A：静止（命令流与相机都不变）—— 纯提交/绘制下限
    const Stats sStatic = measure(N, [&](int) {
        off.execute(renderer.commandList(), renderer.viewFor(camera, w, h));
    });
    reportLine("A 静止", sStatic);

    // B：交互（相机旋转/缩放）—— 用户在视口拖动的真实路径
    const Stats sInteract = measure(N, [&](int i) {
        camera.yaw += 0.01f;
        camera.pitch = 0.25f + 0.15f * std::sin(static_cast<float>(i) * 0.05f);
        camera.distance = radius * (2.2f + 0.3f * std::sin(static_cast<float>(i) * 0.03f));
        off.execute(renderer.commandList(), renderer.viewFor(camera, w, h));
    });
    reportLine("B 交互(相机)", sInteract);
    // 复位相机，后续档位与 B 的取景一致
    camera.centerOn(center, radius);

    // C：轨迹帧切（走坐标重放 —— P4 增量路径），拆出各阶段
    Stats sApply, sPrepare, sExec;
    const Stats sTraj = measure(N, [&](int i) {
        const uint64_t f = static_cast<uint64_t>(i) % traj->frames.size();
        auto t = Clock::now();
        scene->applyTrajectoryFrame(0, f);
        sApply.add(msSince(t));
        t = Clock::now();
        renderer.update(0.0f);
        renderer.prepare();
        sPrepare.add(msSince(t));
        t = Clock::now();
        off.execute(renderer.commandList(), renderer.viewFor(camera, w, h));
        sExec.add(msSince(t));
    });
    reportLine("C 轨迹帧切(重放)", sTraj);
    std::printf("      └ 分解: apply(写坐标) %.2f + prepare(重放) %.2f + execute(GPU) %.2f ms"
                "  [%s]\n",
                sApply.avg(), sPrepare.avg(), sExec.avg(),
                renderer.lastPrepareWasReplay() ? "本次为坐标重放" : "本次为全量重建");

    // D：每帧强制全量重建（对照 C —— 量化坐标重放省下的时间）
    if (rebuildEvery > 0) {
        uint64_t rev = 1;
        const Stats sRebuild = measure(N, [&](int i) {
            if (i % rebuildEvery == 0) {
                reps[0].radiusScale = (i % (2 * rebuildEvery) == 0) ? 1.0f : 1.0001f;
                renderer.bindRepresentations(&reps, ++rev);   // 修订号变 → 全量重建
            }
            const uint64_t f = static_cast<uint64_t>(i) % traj->frames.size();
            scene->applyTrajectoryFrame(0, f);
            renderer.update(0.0f);
            renderer.prepare();
            off.execute(renderer.commandList(), renderer.viewFor(camera, w, h));
        });
        reportLine("D 帧切(全量重建)", sRebuild);
        std::printf("      └ 重放相对全量重建省下 %.1f%% (%.2f -> %.2f ms)\n",
                    100.0 * (sRebuild.avg() - sTraj.avg()) / std::max(sRebuild.avg(), 1e-6),
                    sRebuild.avg(), sTraj.avg());
    }

    // F：界面交互代价对比 —— 只改材质（透明度）vs 改几何参数（等值面/网格间距）。
    // 这是"点 QuickSurf 后调参数会不会卡"的直接度量：
    //   材质改动**不该**重算等值面（缓存应命中），几何改动必须重算。
    double materialEditMs = -1.0, geomEditMs = -1.0;
    if (rebuildEvery > 0 && (reps[0].type == engine::RepresentationType::QuickSurf ||
                             reps[0].type == engine::RepresentationType::Surface)) {
        auto timedEdit = [&](const char* label, auto&& mutate) -> double {
            Stats st;
            for (int i = 0; i < 8; ++i) {
                mutate(i);
                const auto t = Clock::now();
                renderer.bindRepresentations(&reps, 1000 + static_cast<uint64_t>(i));
                renderer.update(0.0f);
                renderer.prepare();
                st.add(msSince(t));
            }
            std::printf("  %-22s avg %7.2f ms  (最大 %7.2f)\n", label, st.avg(),
                        st.max());
            return st.avg();
        };
        materialEditMs = timedEdit("材质改动(透明度)", [&](int i) {
            reps[0].opacity = 0.5f + 0.02f * static_cast<float>(i % 4);
        });
        geomEditMs = timedEdit("几何改动(网格间距)", [&](int i) {
            reps[0].surfGridSpacing = 0.60f + 0.01f * static_cast<float>(i % 4);
        });
    }

    // G：异步表面生成（--async-surface）——量化"点 QuickSurf 时主线程不再阻塞"
    //   主线程 prepare 只提交后台任务（应 ≈ 0 ms），生成在 worker 线程完成；
    //   prepareBlocking 是导出路径的等待量（= 原来主线程要卡住的时间）。
    if (asyncSurface && (reps[0].type == engine::RepresentationType::QuickSurf ||
                         reps[0].type == engine::RepresentationType::Surface)) {
        renderer.setAsyncSurfaceEnabled(true);
        Stats stMain, stWait;
        for (int i = 0; i < 4; ++i) {
            // 每轮换一个未用过的网格间距 → 必然缓存未命中、必须真正重算
            reps[0].surfGridSpacing = 0.66f + 0.01f * static_cast<float>(i);
            renderer.bindRepresentations(&reps, 3000 + static_cast<uint64_t>(i));
            auto t = Clock::now();
            renderer.update(0.0f);
            renderer.prepare();                       // 主线程：提交任务后立即返回
            stMain.add(msSince(t));
            t = Clock::now();
            renderer.prepareBlocking();               // 等一下后台任务 + 同步换入
            stWait.add(msSince(t));
        }
        std::printf("\n[G] 异步表面（主线程不阻塞）\n");
        std::printf("  %-22s avg %7.2f ms  max %7.2f ms  ← 主线程 prepare 的代价\n",
                    "主线程 prepare", stMain.avg(), stMain.max());
        std::printf("  %-22s avg %7.2f ms  max %7.2f ms  ← 后台生成总时长（不占 UI）\n",
                    "等待后台表面", stWait.avg(), stWait.max());
        engine::CommandStats csAsync{};
        engine::commandStats(renderer.commandList(), &csAsync);
        std::printf("  %-22s %u 个网格命令\n", "命令流表面", csAsync.meshes);
        renderer.setAsyncSurfaceEnabled(false);
    }

    // E：含回读（导出路径成本，不适合当 FPS 看）
    if (doReadback) {
        const Stats sRb = measure(std::max(1, N / 4), [&](int) {
            off.execute(renderer.commandList(), renderer.viewFor(camera, w, h));
            off.readbackPixels(px, engine::PixelFormat::Rgba8);
        });
        reportLine("E 含回读(导出)", sRb);
    }

    // —— ④ 资源占用 ——
    const RssInfo rssEnd = readRss();
    const double wallSec = Seconds(Clock::now() - wallBegin).count();
    const double cpuSec = readCpuSeconds() - cpuBegin;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());

    std::printf("\n[4] 资源\n");
    std::printf("  RSS 峰值/当前        : %.1f / %.1f MiB\n", mib(rssEnd.peakBytes),
                mib(rssEnd.curBytes));
    std::printf("  RSS 加载后           : %.1f MiB\n", mib(rssAfterLoad.curBytes));
    if (haveVram) {
        std::printf("  显存 已用/预算       : %.1f / %.1f MiB  (%.1f%%, %u 个 DEVICE_LOCAL 堆)\n",
                    mib(vram.usedBytes), mib(vram.budgetBytes),
                    vram.budgetBytes ? 100.0 * static_cast<double>(vram.usedBytes) /
                                           static_cast<double>(vram.budgetBytes)
                                     : 0.0,
                    vram.heapCount);
    } else {
        std::printf("  显存                 : 不可用（VK_EXT_memory_budget 未启用）\n");
    }
    std::printf("  CPU 时间/墙钟        : %.2f / %.2f s  = %.1f%% (单核基准, 共 %u 核)\n",
                cpuSec, wallSec, wallSec > 0.0 ? 100.0 * cpuSec / wallSec : 0.0, cores);

    // —— ⑤ 机器可读结果（供汇总脚本聚合）——
    std::printf("\nBENCH_JSON {\"rep\":\"%s\",\"w\":%u,\"h\":%u,\"atoms\":%zu,"
                "\"instances\":%zu,\"frames\":%llu,\"hideSolvent\":%s,"
                "\"parseGroMs\":%.2f,\"openXtcMs\":%.2f,\"readFramesMs\":%.2f,"
                "\"attachMs\":%.2f,\"buildCmdMs\":%.2f,\"firstFrameMs\":%.2f,"
                "\"staticAvgMs\":%.3f,\"staticP95Ms\":%.3f,"
                "\"interactAvgMs\":%.3f,\"interactP95Ms\":%.3f,\"interactMaxMs\":%.3f,"
                "\"trajAvgMs\":%.3f,\"trajP95Ms\":%.3f,"
                "\"trajApplyMs\":%.3f,\"trajPrepareMs\":%.3f,\"trajExecMs\":%.3f,"
                "\"spheres\":%u,\"cylinders\":%u,\"meshes\":%u,"
                "\"rssPeakMiB\":%.1f,\"vramUsedMiB\":%.1f,\"vramBudgetMiB\":%.1f,"
                "\"cpuPct\":%.1f,\"cmdPoolBytes\":%u,\"drawCalls\":%u,"
                "\"materialEditMs\":%.2f,\"geomEditMs\":%.2f}\n",
                repName.c_str(), w, h, totalAtoms, scene->instances().size(),
                static_cast<unsigned long long>(traj->frames.size()),
                hideSolvent ? "true" : "false", tParseGro, tOpenXtc, tReadFrames, tAttach,
                tBuildCmd, tFirstFrame, sStatic.avg(), sStatic.pct(0.95), sInteract.avg(),
                sInteract.pct(0.95), sInteract.max(), sTraj.avg(), sTraj.pct(0.95),
                sApply.avg(), sPrepare.avg(), sExec.avg(), cs.spheres, cs.cylinders,
                cs.meshes, mib(rssEnd.peakBytes), mib(vram.usedBytes),
                mib(vram.budgetBytes), wallSec > 0.0 ? 100.0 * cpuSec / wallSec : 0.0,
                renderer.commandList().size(), drawCalls, materialEditMs, geomEditMs);
    return 0;
}
