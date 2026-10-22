// =============================================================================
// MolecuLens · cli · cmd/render/render_cmd.cpp —— 无头渲染（M4）
// 规格来源：主方案 §11.3（离屏渲染 → 图像编码；退出码 3 = 用法错误）
//
// 链路：解析 → 场景 → 命令流（Renderer::prepare，无 GPU）→ 离屏渲染
//       （OffscreenRenderer headless 自建设备）→ RGBA8 回读 → PNG。
// 说明：--depth 16 走"SSAA 浮点求平均 → 量化到 16 bit"路径（与 GUI 导出同一策略），
//       由 writePng16 写出真正的 16-bit PNG。
// =============================================================================

#include "src/cli/cmd/render/render_cmd.h"
#include "engine/export/overlay/esp_overlay_raster.h"   // ESP 色标条/极值标签栅格化

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>   // std::memcpy（16F 半浮点回读缓冲重组）
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/export/anim/anim_encoder.h"   // --anim：PNG 序列 → MP4/WebM/GIF/APNG
#include "engine/export/png/png_encoder.h"
#include "engine/raytrace/ray_tracer.h"   // --rt：CPU 光追出图（P1）
#include "engine/rep/representation.h"
#include "engine/rep/representer.h"
#include "engine/render/render_parameter.h"   // LightParams（--light-az/--light-el）
#include "engine/render/renderer.h"
#include "engine/style/espsurface/esp_surface.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"
#include "entity/scene/scene.h"
#include "entity/trajectory/dcd_reader.h"
#include "entity/trajectory/trr/trr_reader.h"
#include "entity/trajectory/xtc/xtc_reader.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace moleculens::cli {

namespace {

bool parseStyle(const std::string& name, engine::RepresentationType* out) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    using RT = engine::RepresentationType;
    if (s == "wireframe" || s == "line") *out = RT::Wireframe;
    else if (s == "stick") *out = RT::Stick;
    else if (s == "ballstick" || s == "ballandstick") *out = RT::BallAndStick;
    else if (s == "spacefill" || s == "cpk") *out = RT::Spacefill;
    else if (s == "vdw") *out = RT::Vdw;
    else if (s == "cartoon") *out = RT::Cartoon;
    else if (s == "surface") *out = RT::Surface;
    else if (s == "quicksurf") *out = RT::QuickSurf;
    else if (s == "licorice") *out = RT::Licorice;
    else if (s == "tube") *out = RT::Tube;
    else if (s == "newcartoon") *out = RT::NewCartoon;
    else if (s == "ribbon") *out = RT::Ribbon;
    else if (s == "espsurface" || s == "esp") *out = RT::EspSurface;
    else if (s == "plate" || s == "平板") *out = RT::Plate;
    else if (s == "diamond" || s == "gem" || s == "菱形") *out = RT::Diamond;
    else if (s == "dotcloud" || s == "dots" || s == "点云") *out = RT::DotCloud;
    else if (s == "polyhedron" || s == "poly" || s == "多面体")
        *out = RT::Polyhedron;
    else return false;
    return true;
}

// 配色方案（与 GUI 的「配色」下拉一致；name = 原子名配色）
bool parseColorScheme(const std::string& name, engine::ColorScheme* out) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    using CS = engine::ColorScheme;
    if (s == "element" || s == "cpk") *out = CS::ElementCPK;
    else if (s == "chain") *out = CS::Chain;
    else if (s == "residue" || s == "resid") *out = CS::Residue;
    else if (s == "bfactor" || s == "beta") *out = CS::BFactor;
    else if (s == "uniform" || s == "single") *out = CS::Uniform;
    else if (s == "name") *out = CS::AtomName;
    else return false;
    return true;
}

// 多表示规格解析：style[:selection[:color[:opacity]]]
void splitColon(const std::string& s, std::vector<std::string>* out) {
    out->clear();
    std::string cur;
    for (char c : s) {
        if (c == ':') {
            out->push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out->push_back(cur);
}

// 相机预设（对齐到包围球正面 / 侧面 / 俯视）
void applyCameraPreset(engine::OrbitCamera& cam, const std::string& preset) {
    std::string s = preset;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "side") {
        cam.yaw = 1.5707963f;    // 90°
        cam.pitch = 0.0f;
    } else if (s == "top") {
        cam.yaw = 0.0f;
        cam.pitch = 1.5533431f;  // 89°（避免与 up 向量退化）
    } else {                     // front（默认）
        cam.yaw = 0.0f;
        cam.pitch = 0.0f;
    }
}

// 把一帧"体系级"坐标（XTC/DCD/TRR 的原子顺序）按实例切片写回场景。
// 关键两步（与 SceneStructure::applyTrajectoryFrame 完全一致）：
//   ① 逐分子 PBC 补齐（unwrapAroundFirst）：轨迹坐标同样是 wrapped 的，不补齐
//      就会把跨盒子的分子画成 ~盒长 的长棍，播放时看起来就是"一闪一闪"；
//   ② 再乘实例世界变换（多体系并排时才有非单位变换）。
// 旧实现只写 instance 0（一个水分子），其余实例保持首帧坐标 → 整片溶剂不动。
void applySystemFrameToScene(entity::SceneStructure& scene,
                             const std::vector<glm::vec3>& positions,
                             const std::array<float, 9>& box) {
    const auto& insts = scene.instances();
    const auto& structs = scene.structures();
    size_t offset = 0;
    std::vector<glm::vec3> slice;
    for (uint32_t i = 0; i < insts.size(); ++i) {
        const auto& st = structs[insts[i].structureIndex];
        const size_t n = st.elements.size();
        if (offset + n > positions.size()) {
            break;
        }
        slice.resize(n);
        for (size_t k = 0; k < n; ++k) {
            slice[k] = positions[offset + k];
        }
        entity::unwrapAroundFirst(slice.data(), slice.size(), box);
        // 分子整体回盒：unwrapped 轨迹播放久了分子会"扩散出盒"（画面散开），
        // 显示上应把每个分子的首原子折回盒子，其余原子跟着平移 —— 分子始终完整，
        // 整体位置仍按轨迹走（跨盒瞬间会有一次整分子跳变，这是常规视角处理）。
        const float L[3] = {box[0], box[4], box[8]};
        glm::vec3 shift(0.0f);
        for (int a = 0; a < 3; ++a) {
            if (L[a] > 1e-3f) {
                shift[a] = -std::floor(slice[0][a] / L[a]) * L[a];
            }
        }
        for (size_t k = 0; k < n; ++k) {
            slice[k] = insts[i].transform.apply(slice[k] + shift);
        }
        scene.writeFramePositions(i, slice);
        offset += n;
    }
}

// 时间轴 PBC 展开（分子级）：以上一帧连续位置为基准推进锚点，消除相邻帧
// "跨盒跳一个盒长"。只用于**连续缓冲**；显示前再由 applySystemFrameToScene
// 把每个分子整体折回盒子（否则扩散会让画面散开）。
void unwrapAcrossTime(entity::SceneStructure& scene,
                      const std::vector<glm::vec3>& raw,
                      const std::array<float, 9>& box,
                      std::vector<glm::vec3>* prev,
                      std::vector<glm::vec3>* out) {
    out->assign(raw.begin(), raw.end());
    const auto& insts = scene.instances();
    const auto& structs = scene.structures();
    const float L[3] = {box[0], box[4], box[8]};
    const bool firstFrame = prev->empty();
    size_t offset = 0;
    for (uint32_t i = 0; i < insts.size(); ++i) {
        const size_t n = structs[insts[i].structureIndex].elements.size();
        if (offset + n > raw.size()) {
            break;
        }
        if (firstFrame) {
            entity::unwrapAroundFirst(out->data() + offset, n, box);
        } else {
            const glm::vec3 d =
                entity::minimumImageDelta(raw[offset] - (*prev)[offset], L);
            const glm::vec3 base = (*prev)[offset] + d;
            for (size_t k = 0; k < n; ++k) {
                (*out)[offset + k] =
                    base + entity::minimumImageDelta(raw[offset + k] - base, L);
            }
        }
        offset += n;
    }
    *prev = *out;
}

// 盒式降采样（SSAA）：src 为 w*factor × h*factor
std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h,
                                uint32_t factor) {
    const uint32_t rw = w * factor;   // 源图行宽（源高为 h * factor）
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    const uint32_t n = factor * factor;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t dy = 0; dy < factor; ++dy) {
                for (uint32_t dx = 0; dx < factor; ++dx) {
                    const size_t s = (static_cast<size_t>(y * factor + dy) * rw +
                                      (x * factor + dx)) * 4;
                    if (s + 3 >= src.size()) {
                        continue;
                    }
                    r += src[s];
                    g += src[s + 1];
                    b += src[s + 2];
                    a += src[s + 3];
                }
            }
            const size_t d = (static_cast<size_t>(y) * w + x) * 4;
            out[d] = static_cast<uint8_t>(r / n);
            out[d + 1] = static_cast<uint8_t>(g / n);
            out[d + 2] = static_cast<uint8_t>(b / n);
            out[d + 3] = static_cast<uint8_t>(a / n);
        }
    }
    return out;
}

} // namespace

int renderCommand(CLI::App& app) {
    // static：callback 在 CLI11_PARSE（main）时执行，函数已返回——局部变量悬垂
    static std::string input, out = "out.png";
    static std::string repr = "ballstick";
    static std::string colorName;           // 空 = 自动（线框默认按原子名，其余元素 CPK）
    static std::vector<std::string> repSpecs;   // --rep 可重复：style[:sel[:color[:opacity]]]
    static int width = 1920, height = 1080;
    static bool systemSurface = false;   // --system-surface：整体系合并为一个表面（G5）
    static int meshDetail = 3;      // 球体细分档（1=80 … 4=5120 面）
static int rtSpp = 0;                  // --rt：>0 时改用光追出图（P1 原型）
    static std::string rtBackend = "auto";   // --rt-backend cpu|gpu|auto
    static int meshSegments = 24;   // 键圆柱截面段数
    static float zoom = 1.0f;       // 相机拉近倍数（用于验证视锥剔除）
    static std::string camera = "front";
    static int scale = 1, msaa = 4;
    static std::string bg = "current";
    // 渲染风格预设：default / soft / publication / vivid（见 engine/render_parameter.h）。
    // publication 会同时切到白底 + 高调柔光 + 关闭阴影，适合论文插图。
    static std::string styleName = "default";
    // 剖切面（v2.3）：--clip x|-x|y|-y|z|-z，--clip-offset 为沿法线的位置（Å）
    static std::string clipAxis;
    static float clipOffset = 0.0f;
    // 统一色（v2.3）：--color "#RRGGBB"，给基础表示一个低饱和主色（表面/球棍均可）
    static std::string baseColor;
    static int depth = 8;
    // 方向光角度（度）：999 = 用场景默认。加这两个开关是为了能复现/验证
    // "壳内分子把影子投到半透明壳上" 这类只在特定光向下可见的缺陷。
    static float lightAzDeg = 999.0f;
    static float lightElDeg = 999.0f;
    // —— ESP 映射表面（density 定几何 / ESP cube 定颜色）——
    static std::string espCube;          // 静电势 cube 路径；提供则叠加 ESP 表面
    static std::string densityCube;      // 密度 cube 路径（ESP 表面几何；可缺省）
    static std::string espCmap = "reference";   // 色标方案（默认参考截图配色）
    static float isoLevel = 0.001f;      // 密度等值面阈值（a.u.）
    static float espVmin = -45.0f;       // 色标下限（kcal/mol）
    static float espVmax = 45.0f;        // 色标上限（kcal/mol）
    static bool  espNoAutoRange = false; // --no-auto-range：使用固定 vmin/vmax
    static float espOpacity = 0.85f;     // 表面不透明度
    static float espDiffuse = 0.90f;     // 表面漫反射强度（烘进顶点色，控制颜色深浅）
    static float espAmbient = 0.30f;     // 表面环境光
    static float espSpecular = 0.15f;    // 表面高光
    static float espShininess = 48.0f;   // 表面高光指数
    static bool  espInvert = false;      // 翻转 ESP 色标方向
    static float baseOpacity = 1.0f;     // 基础表示不透明度（测试/透明表面用）
    static std::string materialName;     // 材质预设名（Opaque/Glass1/Glass2/...）
    // 表面/快速表面参数（<=0 表示沿用 Representation 默认值）
    static float surfIso = -1.0f;        // 等值面阈值
    static float surfGrid = -1.0f;       // 网格间距（Å；越小越精细越慢）
    // 轨迹：把指定帧的坐标写进场景后再渲染（DCD/XTC/TRR）
    static std::string trajPath;         // 轨迹文件；空则渲染结构文件本身
    static int trajFrame = 0;            // 帧号（0 起；-1 = 末帧）
    // 动画导出：逐帧渲染后交给 ffmpeg 封装（扩展名决定格式）
    static std::string animOut;          // 输出文件（.mp4/.webm/.gif/.apng）
    static int animFps = 30;             // 动画帧率
    static std::string animRange;        // 帧范围 a:b（空 = 全部帧）
    static int animStep = 1;             // 帧步长
    static int animInterp = 1;           // 帧间线性插值倍数（1 = 关）

    auto* sub = app.add_subcommand("render", "无头渲染分子为图像");
    sub->add_option("input", input, "输入结构文件 (.pdb/.gro)")->required();
    sub->add_option("--repr", repr, "表示风格 (wireframe/stick/ballstick/spacefill/...)");
    sub->add_option("--coloring", colorName,
                    "配色方法 (element/chain/residue/bfactor/uniform/name；"
                    "name = 原子名配色：碳青/氮蓝/氧红/氢白)");
    sub->add_option("--rep", repSpecs,
                    "多表示（可重复），格式 style[:selection[:color[:opacity]]]，"
                    "例如 --rep 'quicksurf:all:name:0.25' --rep 'surface:not resname SOL:name'");
    sub->add_option("--material", materialName,
                    "材质预设名（Opaque/Transparent/Glass1/Glass2/Glass3/Diffuse/"
                    "Glossy/...；缺省 Opaque。玻璃类需配合 --opacity 才透）");
    sub->add_flag("--system-surface", systemSurface,
                  "整体系合并表面：Surface/QuickSurf 只生成一个表面（含溶剂）");
    sub->add_option("--surf-iso", surfIso,
                    "表面等值面阈值（surface/quicksurf，缺省用内置 0.5）");
    sub->add_option("--surf-grid", surfGrid,
                    "表面网格间距 Å（越小越精细越慢，缺省 0.5）");
    sub->add_option("--size", width, "宽度")->default_val(1920);
    sub->add_option("--height", height, "高度")->default_val(1080);
    sub->add_option("--detail", meshDetail,
                    "球体细分档 1-4（1=80/2=320/3=1280/4=5120 面；预览默认 3）")
        ->default_val(3);
    sub->add_option("--seg", meshSegments, "键圆柱截面段数（预览默认 24，导出可用 48）")
        ->default_val(24);
    sub->add_option("--zoom", zoom, "相机拉近倍数（>1 放大；用于验证视锥剔除）")
        ->default_val(1.0f);
    sub->add_option("--rt", rtSpp, "光追采样数（>0 时改用光追出图；1 = 单样本，4 = 抗锯齿）")->default_val(0);
    sub->add_option("--rt-backend", rtBackend,
                    "光追后端：cpu = CPU 光线追踪；gpu = Vulkan KHR 硬件光追（需 RT 设备）；"
                    "auto = 有 RT 设备用 GPU，否则 CPU")
        ->default_val("auto");
    sub->add_option("--camera", camera, "相机预设 (front/side/top)");
    sub->add_option("--scale", scale, "超采样倍数（SSAA）")->default_val(1);
    sub->add_option("--msaa", msaa, "MSAA（离屏恒 1x，保留参数）")->default_val(4);
    sub->add_option("--bg", bg, "背景 (current/white/transparent)")->default_val("current");
    sub->add_option("--style", styleName,
                    "渲染风格预设 (default/soft/publication/vivid)；publication 自动白底+柔光+无阴影")
        ->default_val("default");
    sub->add_option("--clip", clipAxis,
                    "剖切面轴向 (x/-x/y/-y/z/-z)；切开分子/表面观察内部");
    sub->add_option("--clip-offset", clipOffset,
                    "剖切面沿法线的位置 Å（保留 <= 该位置的一侧）")
        ->default_val(0.0f);
    sub->add_option("--color", baseColor,
                    "统一色 #RRGGBB（覆盖基础表示的元素配色；ESP/轨道表示不受影响）");
    sub->add_option("--depth", depth, "位深 (8/16)")->default_val(8);
    sub->add_option("--light-az", lightAzDeg,
                    "方向光方位角（度；0 = +Z 正对相机，180 = 背光；缺省用场景默认）");
    sub->add_option("--light-el", lightElDeg,
                    "方向光仰角（度；90 = 正上方；缺省用场景默认）");
    sub->add_option("--esp", espCube,
                    "静电势 cube 文件（Multiwfn/Gaussian ESP.cub）；提供后叠加 ESP 映射表面");
    sub->add_option("--density", densityCube,
                    "密度 cube 文件（ESP 表面几何）；缺省时用 --esp 文件本身");
    sub->add_option("--iso", isoLevel, "密度等值面阈值 (a.u.，默认 0.001 ≈ vdW 表面)")
        ->default_val(0.001);
    sub->add_option("--vmin", espVmin, "ESP 色标下限 (kcal/mol，--no-auto-range 时生效)")
        ->default_val(-45.0);
    sub->add_option("--vmax", espVmax, "ESP 色标上限 (kcal/mol，--no-auto-range 时生效)")
        ->default_val(45.0);
    sub->add_flag("--no-auto-range", espNoAutoRange,
                  "关闭 ESP 色标自动范围（改用 --vmin/--vmax）");
    sub->add_flag("--invert", espInvert, "翻转 ESP 色标方向（低值端 ↔ 高值端）");
    sub->add_option("--cmap", espCmap,
                    "ESP 色标方案 (reference/spectral/rainbow/rwb/bwr/coolwarm/turbo/viridis/jet)")
        ->default_val("reference");
    sub->add_option("--esp-opacity", espOpacity, "ESP 表面不透明度 (0.05~1.0)")
        ->default_val(0.85);
    sub->add_option("--esp-diffuse", espDiffuse,
                    "ESP 表面漫反射强度/颜色深浅 (0.2~1.0)")
        ->default_val(0.90);
    sub->add_option("--esp-ambient", espAmbient, "ESP 表面环境光 (0.0~1.0)")
        ->default_val(0.30);
    sub->add_option("--esp-specular", espSpecular, "ESP 表面高光强度 (0.0~1.0)")
        ->default_val(0.15);
    sub->add_option("--esp-shininess", espShininess, "ESP 表面高光指数")
        ->default_val(48.0);
    sub->add_option("--opacity", baseOpacity, "基础表示不透明度 (0.05~1.0)")
        ->default_val(1.0);
    sub->add_option("--traj", trajPath,
                    "轨迹文件 (.dcd/.xtc/.trr)；提供后渲染其中一帧而非结构文件首帧");
    sub->add_option("--frame", trajFrame, "轨迹帧号（0 起；-1 = 末帧）")->default_val(0);
    sub->add_option("--anim", animOut,
                    "导出轨迹动画（扩展名决定格式：.mp4/.webm/.gif/.apng）；需配合 --traj");
    sub->add_option("--anim-fps", animFps, "动画帧率")->default_val(30);
    sub->add_option("--anim-range", animRange,
                    "动画帧范围 a:b（缺省 = 全部帧；-1 表示末帧）");
    sub->add_option("--anim-step", animStep, "动画帧步长")->default_val(1);
    sub->add_option("--anim-interp", animInterp,
                    "帧间线性插值倍数（1=关；2-4 可让 10 ps 帧距的轨迹播放更顺滑）")
        ->default_val(1);
    sub->add_option("--out", out, "输出路径")->default_val("out.png");

    sub->callback([&]() {
        (void)msaa;   // 离屏 FBO 附件恒为 1x（管线/渲染通道一致性的硬约束）

        if (width <= 0 || height <= 0) {
            spdlog::error("[cli] render: 非法尺寸 {}x{}", width, height);
            std::exit(3);
        }
        if (depth != 8 && depth != 16) {
            spdlog::error("[cli] render: --depth {} 非法（仅 8 或 16）", depth);
            std::exit(3);
        }
        if (rtBackend != "cpu" && rtBackend != "gpu" && rtBackend != "auto") {
            spdlog::error("[cli] render: --rt-backend '{}' 非法（cpu/gpu/auto）",
                          rtBackend);
            std::exit(3);
        }

        // —— 1. 解析 ——
        auto parsed = entity::ParserRegistry::defaultRegistry().parseFile(
            input, entity::ParseOptions{});
        if (!parsed) {
            spdlog::error("[cli] render: 解析失败: {}", parsed.errorMessage());
            std::exit(3);
        }
        if (!parsed->scene || parsed->scene->instances().empty()) {
            spdlog::error("[cli] render: 输入不含任何实例");
            std::exit(3);
        }

        // —— 1.5 轨迹帧（可选）：把指定帧的世界坐标写进场景后再渲染 ——
        // 读取器在动画导出（--anim）里还要复用，因此声明在 if 之外。
        entity::TrrReader trr;
        entity::XtcReader xtc;
        entity::DcdReader dcd;
        entity::ITrajectoryReader* rdr = nullptr;
        uint64_t trajTotal = 0;
        if (!trajPath.empty()) {
            if (trr.canOpen(trajPath)) {
                rdr = &trr;
            } else if (xtc.canOpen(trajPath)) {
                rdr = &xtc;
            } else if (dcd.canOpen(trajPath)) {
                rdr = &dcd;
            }
            if (rdr == nullptr) {
                spdlog::error("[cli] render: 无法识别轨迹格式（支持 .dcd/.xtc/.trr）: {}",
                              trajPath);
                std::exit(3);
            }
            auto opened = rdr->open(trajPath);
            if (!opened) {
                spdlog::error("[cli] render: 轨迹打开失败: {}", opened.errorMessage());
                std::exit(3);
            }
            const uint64_t frames = rdr->frameCount();
            trajTotal = frames;
            uint64_t frame = 0;
            if (trajFrame < 0) {
                frame = frames > 0 ? frames - 1 : 0;
            } else {
                frame = static_cast<uint64_t>(trajFrame);
            }
            auto ts = rdr->readFrame(frame);
            if (!ts) {
                spdlog::error("[cli] render: 读取轨迹帧 {} 失败: {}", frame, ts.errorMessage());
                std::exit(3);
            }
            applySystemFrameToScene(*parsed->scene, ts->positions, ts->box);
            spdlog::info("[cli] render: 使用轨迹帧 {}/{}（{} 原子）", frame, frames,
                         ts->positions.size());
        }

        // —— 1.6 方向光角度（可选）：覆盖场景默认光向 ——
        // 用途：复现只在特定光向下可见的渲染缺陷（例如壳内分子把影子投到
        // 半透明壳上形成的"幽灵"），也便于出图时统一光位。
        if (lightAzDeg < 900.0f || lightElDeg < 900.0f) {
            engine::LightParams lp;
            lp.setDirLightAngles(glm::radians(lightAzDeg < 900.0f ? lightAzDeg : 33.7f),
                                 glm::radians(lightElDeg < 900.0f ? lightElDeg : 48.0f));
            auto& props = parsed->scene->properties();
            for (int i = 0; i < 3; ++i) {
                props.dirLightDir[i] = lp.dirLightDir[i];
            }
            parsed->scene->setDirty(entity::DIRTY_PROPERTIES);
            spdlog::info("[cli] render: 方向光 az={}° el={}°",
                         lightAzDeg < 900.0f ? lightAzDeg : 33.7f,
                         lightElDeg < 900.0f ? lightElDeg : 48.0f);
        }

        // —— 2. 场景 → 命令流 ——
        engine::RepresentationType type = engine::RepresentationType::BallAndStick;
        if (!parseStyle(repr, &type)) {
            spdlog::error("[cli] render: 未知风格 '{}'", repr);
            std::exit(3);
        }
        engine::ColorScheme colorScheme = engine::ColorScheme::ElementCPK;
        // 白底/透明底导出时，氢的白色会隐没 -> 让配色逻辑压暗氢
        engine::Representer::setLightBackground(bg == "white" || bg == "transparent");
        if (!colorName.empty()) {
            if (!parseColorScheme(colorName, &colorScheme)) {
                spdlog::error("[cli] render: 未知配色 '{}'", colorName);
                std::exit(3);
            }
        } else if (type == engine::RepresentationType::Wireframe) {
            // 未指定配色时：线框默认用按原子名配色（与 GUI 默认策略一致）
            colorScheme = engine::ColorScheme::AtomName;
        }
        // ESP 表面：--esp 提供静电势 cube 时追加一条 EspSurface 表示
        // （密度 cube = 输入结构文件；颜色来自 ESP cube）。
        if (!espCube.empty() && !std::filesystem::exists(espCube)) {
            spdlog::error("[cli] render: 静电势 cube 不存在: {}", espCube);
            std::exit(3);
        }
        static std::vector<engine::Representation> reps;
        reps.clear();
        if (!repSpecs.empty()) {
            // 多表示：每条 --rep 生成一条表示（顺序即绘制顺序；透明层放前面）
            int repIdx = 0;
            for (const auto& spec : repSpecs) {
                std::vector<std::string> fields;
                splitColon(spec, &fields);
                engine::Representation rep;
                rep.name = "cli-" + std::to_string(repIdx++);
                if (fields.empty() || !parseStyle(fields[0], &rep.type)) {
                    spdlog::error("[cli] render: --rep 规格里未知风格 '{}'", spec);
                    std::exit(3);
                }
                if (fields.size() > 1) {
                    rep.selection = fields[1];
                }
                if (fields.size() > 2 && !fields[2].empty() &&
                    !parseColorScheme(fields[2], &rep.colorScheme)) {
                    spdlog::error("[cli] render: --rep 规格里未知配色 '{}'", fields[2]);
                    std::exit(3);
                }
                if (fields.size() > 3 && !fields[3].empty()) {
                    try {
                        rep.opacity = std::clamp(std::stof(fields[3]), 0.05f, 1.0f);
                    } catch (const std::exception&) {
                        spdlog::error("[cli] render: --rep 规格里非法不透明度 '{}'", fields[3]);
                        std::exit(3);
                    }
                }
                if (fields.size() > 4 && !fields[4].empty()) {
                    const int mi = engine::materialPresetIndexByName(fields[4].c_str());
                    if (mi < 0) {
                        spdlog::error("[cli] render: --rep 规格里未知材质 '{}'", fields[4]);
                        std::exit(3);
                    }
                    rep.materialPreset = mi;
                }
                if (surfIso > 0.0f) {
                    rep.surfIsovalue = surfIso;
                }
                if (surfGrid > 0.0f) {
                    rep.surfGridSpacing = surfGrid;
                }
                // --system-surface 对 --rep 生成的每条表示同样生效（此前只作用于 --repr，
                // 导致「多表示 + 整体系表面」的组合静默退化成逐分子表面）
                rep.systemSurface = systemSurface;
                reps.push_back(rep);
            }
        } else if (type != engine::RepresentationType::EspSurface) {
            engine::Representation rep;
            rep.name = "cli";
            rep.type = type;
            rep.opacity = std::clamp(baseOpacity, 0.05f, 1.0f);
            // 配色（v2.3）：element / chain / residue / bfactor / uniform / name
            rep.colorScheme = colorScheme;
            rep.systemSurface = systemSurface;
            // 表面/快速表面的等值面与网格质量（仅在这两种风格下有意义）
            if (surfIso > 0.0f) {
                rep.surfIsovalue = surfIso;
            }
            if (surfGrid > 0.0f) {
                rep.surfGridSpacing = surfGrid;
            }
            reps.push_back(rep);
        }
        if (!espCube.empty()) {
            engine::Representation esp;
            esp.name = "cli-esp";
            esp.type = engine::RepresentationType::EspSurface;
            esp.espDensityCubePath =
                std::filesystem::absolute(densityCube.empty() ? espCube : densityCube)
                    .string();
            esp.espPotentialCubePath = std::filesystem::absolute(espCube).string();
            esp.espIsovalue = isoLevel;
            esp.espVminKcal = espVmin;
            esp.espVmaxKcal = espVmax;
            esp.espAutoRange = !espNoAutoRange;
            esp.espInvert = espInvert;
            esp.espColormap = static_cast<int32_t>(engine::espColormapFromName(espCmap));
            // 半透明表面：颜色完全来自逐顶点 ESP；材质给中性白 + 较强高光（更像
            // 参考图的玻璃质感；ambient 略低让明暗对比更明显；diffuse 控制亮度：
            // 顶点色已乘 diffuse，过高会被光照刷白、颜色发灰）。
            esp.opacity = std::clamp(espOpacity, 0.05f, 1.0f);
            esp.ambient[0] = esp.ambient[1] = esp.ambient[2] =
                std::clamp(espAmbient, 0.0f, 1.0f);
            esp.ambient[3] = 1.0f;
            esp.diffuse[0] = esp.diffuse[1] = esp.diffuse[2] =
                std::clamp(espDiffuse, 0.2f, 1.0f);
            esp.diffuse[3] = 1.0f;
            esp.specular[0] = esp.specular[1] = esp.specular[2] =
                std::clamp(espSpecular, 0.0f, 1.0f);
            esp.specular[3] = 1.0f;
            esp.shininess = std::max(1.0f, espShininess);
            esp.roughness = 0.30f;
            reps.push_back(esp);
        }
        if (reps.empty()) {
            spdlog::error(
                "[cli] render: --repr espsurface 需要同时提供 --esp <静电势 cube>");
            std::exit(3);
        }

        // —— 统一色（v2.3）：基础表示改 ColorScheme::Uniform + 低饱和主色 ——
        if (!baseColor.empty() && !reps.empty()) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            bool ok = baseColor.size() == 7 && baseColor[0] == '#';
            int v[6] = {0, 0, 0, 0, 0, 0};
            for (int i = 0; ok && i < 6; ++i) {
                v[i] = nib(baseColor[static_cast<size_t>(i + 1)]);
                ok = v[i] >= 0;
            }
            if (!ok) {
                spdlog::error("[cli] render: --color 需要 #RRGGBB，收到 '{}'", baseColor);
                std::exit(3);
            }
            const float c[3] = {
                static_cast<float>(v[0] * 16 + v[1]) / 255.0f,
                static_cast<float>(v[2] * 16 + v[3]) / 255.0f,
                static_cast<float>(v[4] * 16 + v[5]) / 255.0f};
            reps[0].colorScheme = engine::ColorScheme::Uniform;
            for (int i = 0; i < 3; ++i) {
                reps[0].diffuse[i] = c[i];
                reps[0].ambient[i] = c[i] * 0.55f;
            }
            spdlog::info("[cli] render: 统一色 {}（只作用于基础表示）", baseColor);
        }

        // —— 材质预设默认值（v2.4）：--material 对所有未在 --rep 里显式指定材质的表示生效 ——
        if (!materialName.empty()) {
            const int mi = engine::materialPresetIndexByName(materialName.c_str());
            if (mi < 0) {
                spdlog::error(
                    "[cli] render: 未知材质 '{}'（示例：Opaque/Transparent/Glass1/Glass2/"
                    "Glass3/Diffuse/Glossy）",
                    materialName);
                std::exit(3);
            }
            int applied = 0;
            for (auto& r : reps) {
                if (r.materialPreset == 0) {  // 0 = Opaque（未被 --rep 第 5 段覆盖）
                    r.materialPreset = mi;
                    ++applied;
                }
            }
            spdlog::info("[cli] render: 材质 {}（index {}，作用于 {} 个表示）", materialName,
                         mi, applied);
        }

        engine::Renderer renderer;
        renderer.setScene(parsed->scene.get());
        renderer.bindRepresentations(&reps, 1);

        // —— 渲染风格预设（v2.2）——
        // 风格只进 GlobalUBO（不重建命令流），必须在 viewFor 之前写进 renderer。
        // publication 同时改光照/背景：单开"描边"而没有白底高调光，看起来只是
        // 一圈脏边；预设把两者绑在一起才是可复现的出图配置。
        const engine::RenderStylePreset stylePreset =
            engine::renderStylePresetFromKey(styleName);
        renderer.setRenderStyle(engine::renderStylePreset(stylePreset));
        if (stylePreset == engine::RenderStylePreset::Publication) {
            auto& props = parsed->scene->properties();
            props.skyColor[0] = props.skyColor[1] = props.skyColor[2] = 1.0f;
            props.groundColor[0] = 0.90f;
            props.groundColor[1] = 0.90f;
            props.groundColor[2] = 0.94f;
            props.ambientColor[0] = 0.95f;
            props.ambientColor[1] = 0.95f;
            props.ambientColor[2] = 1.0f;
            props.ambient = 0.34f;           // 高调环境光（比 0.42 收一档，避免洗白）
            props.dirLightIntensity = 0.55f; // 主光保留造型
            props.shadowsEnabled = false;    // 出版图普遍不用硬阴影
            if (bg == "current") {
                bg = "white";
            }
            parsed->scene->setDirty(entity::DIRTY_PROPERTIES);
            spdlog::info("[cli] render: 风格 publication（白底 / 柔光 / 关闭阴影）");
        } else if (stylePreset == engine::RenderStylePreset::Soft) {
            auto& props = parsed->scene->properties();
            props.ambient = std::max(props.ambient, 0.55f);
            parsed->scene->setDirty(entity::DIRTY_PROPERTIES);
        } else if (stylePreset == engine::RenderStylePreset::Vivid) {
            auto& props = parsed->scene->properties();
            props.ambient = std::min(props.ambient, 0.38f);
            parsed->scene->setDirty(entity::DIRTY_PROPERTIES);
        }

        // —— 剖切面（v2.3）：世界空间平面，保留 <= offset 一侧 ——
        if (!clipAxis.empty()) {
            glm::vec3 n(1.0f, 0.0f, 0.0f);
            if (clipAxis == "-x" || clipAxis == "-X") {
                n = glm::vec3(-1.0f, 0.0f, 0.0f);
            } else if (clipAxis == "x" || clipAxis == "X") {
                n = glm::vec3(1.0f, 0.0f, 0.0f);
            } else if (clipAxis == "-y" || clipAxis == "-Y") {
                n = glm::vec3(0.0f, -1.0f, 0.0f);
            } else if (clipAxis == "y" || clipAxis == "Y") {
                n = glm::vec3(0.0f, 1.0f, 0.0f);
            } else if (clipAxis == "-z" || clipAxis == "-Z") {
                n = glm::vec3(0.0f, 0.0f, -1.0f);
            } else if (clipAxis == "z" || clipAxis == "Z") {
                n = glm::vec3(0.0f, 0.0f, 1.0f);
            } else {
                spdlog::error("[cli] render: 未知剖切轴 '{}'（x/-x/y/-y/z/-z）", clipAxis);
                std::exit(3);
            }
            auto& props = parsed->scene->properties();
            props.clipEnabled = true;
            props.clipPlane[0] = n.x;
            props.clipPlane[1] = n.y;
            props.clipPlane[2] = n.z;
            props.clipPlane[3] = -clipOffset;
            parsed->scene->setDirty(entity::DIRTY_PROPERTIES);
            spdlog::info("[cli] render: 剖切面 n=({:.2f},{:.2f},{:.2f}) offset={} Å",
                         n.x, n.y, n.z, clipOffset);
        }
        renderer.prepare();

        // 相机：自动取景后套用预设
        engine::OrbitCamera cam;
        glm::vec3 center(0.0f);
        float radius = 1.0f;
        if (renderer.sceneBounds(&center, &radius)) {
            cam.centerOn(center, radius);
        }
        applyCameraPreset(cam, camera);
        if (zoom > 0.0f) {
            cam.distance /= zoom;   // 拉近：验证视锥剔除（大部分实例移出视野）
        }


        // —— 2.9 可选：CPU 光追出图（P1 原型，--rt N）——
        // 用途：验证 BVH 加速与光追画质；图元来自同一条命令流，因此与光栅化
        //       路径共用材质/颜色，可直接对比。
        if (rtSpp > 0) {
            engine::RtScene rtScene;
            const auto tScene0 = std::chrono::steady_clock::now();
            if (!engine::buildRtScene(renderer.commandList(), rtScene)) {
                spdlog::error("[cli] render: 光追场景构建失败");
                std::exit(3);
            }
            const auto sceneMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - tScene0)
                                     .count();
            engine::RtCamera rtc;
            rtc.position = cam.position();
            rtc.target = cam.target;
            rtc.up = glm::vec3(0.0f, 1.0f, 0.0f);
            rtc.fovDeg = cam.fov;
            rtc.width = static_cast<uint32_t>(width);
            rtc.height = static_cast<uint32_t>(height);
            engine::RtLight rtl;   // 默认半球光 + 方向光（与 GPU 侧默认预设一致）
            glm::vec3 bgColor(0.05f, 0.05f, 0.08f);
            if (bg == "white") {
                bgColor = glm::vec3(1.0f);
            }
            std::vector<uint8_t> rgba;
            const auto t0 = std::chrono::steady_clock::now();
            // 后端选择：cpu = 强制 CPU；gpu = 强制 GPU（不可用即报错）；
            // auto = 有 RT 设备用 GPU，否则 CPU（无 RT 机器自动回退）。
            bool useGpu = false;
            if (rtBackend == "gpu") {
                if (!renderer.gpuRayTraceAvailable()) {
                    spdlog::error(
                        "[cli] render: --rt-backend gpu 但当前设备不可用硬件光追");
                    std::exit(3);
                }
                useGpu = true;
            } else if (rtBackend == "auto") {
                useGpu = renderer.gpuRayTraceAvailable();
            }
            bool traced = false;
            bool usedGpu = false;
            if (useGpu) {
                traced = renderer.gpuRayTrace(rtScene, rtc, rtl, bgColor, rtSpp, rgba);
                usedGpu = traced;
                if (!traced) {
                    if (rtBackend == "gpu") {
                        spdlog::error("[cli] render: GPU 光追渲染失败");
                        std::exit(3);
                    }
                    spdlog::warn("[cli] render: GPU 光追失败 -> 回退 CPU 光追");
                }
            }
            if (!traced) {
                if (!engine::rayTrace(rtScene, rtc, rtl, bgColor, rtSpp, rgba)) {
                    spdlog::error("[cli] render: 光追失败");
                    std::exit(3);
                }
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            spdlog::info("[cli] rt: {} 球 + {} 胶囊 + {} 三角, {} spp, {}x{}, {} ms（场景解包 {} ms, backend={}）",
                         rtScene.spheres.size(), rtScene.cylinders.size(),
                         rtScene.triangles.size(), rtSpp, width, height, ms, sceneMs,
                         usedGpu ? "gpu" : "cpu");
            if (!engine::writePng(out, rtc.width, rtc.height, rgba)) {
                spdlog::error("[cli] render: PNG 写出失败: {}", out);
                std::exit(3);
            }
            spdlog::info("[cli] render: {} -> {} ({}x{}, rt={} spp)", input, out, width,
                         height, rtSpp);
            return;
        }
        // —— 3. 离屏渲染（headless 自建设备）——
        const int ss = std::max(1, scale);
        const uint32_t rw = static_cast<uint32_t>(width * ss);
        const uint32_t rh = static_cast<uint32_t>(height * ss);

        engine::RenderConfig cfg;
        cfg.debug = false;
        cfg.msaaSamples = 0;
        engine::OffscreenRenderer off;
        std::string err;
        if (!off.init(cfg, rw, rh, &err)) {
            spdlog::error("[cli] render: 离屏渲染器初始化失败: {}", err);
            std::exit(3);
        }
        // 网格质量档：默认 3/24（= 预览）；--detail 4 --seg 48 为高倍放大导出用
        off.setMeshQuality(meshDetail, meshSegments);

        // 背景：white = 纯白；transparent = 真透明（清屏 alpha=0，几何写 alpha=1）
        const glm::vec4 bgSolid(0.05f, 0.05f, 0.08f, 1.0f);
        if (bg == "transparent") {
            const float white[3] = {1.0f, 1.0f, 1.0f};
            off.setBackground(white, 0.0f);
        } else if (bg == "white") {
            const float white[3] = {1.0f, 1.0f, 1.0f};
            off.setBackground(white);
        } else {
            off.setBackground(&bgSolid[0]);
        }

        engine::RenderView view = renderer.viewFor(cam, rw, rh);

        // —— 2.95 动画导出（--anim）：逐帧改坐标 → 渲染 → PNG 序列 → ffmpeg ——
        if (!animOut.empty()) {
            if (rdr == nullptr) {
                spdlog::error("[cli] render: --anim 需要同时提供 --traj");
                std::exit(3);
            }
            if (depth == 16) {
                spdlog::warn("[cli] render: 动画导出固定 8-bit（忽略 --depth 16）");
            }
            uint64_t from = 0;
            uint64_t to = trajTotal > 0 ? trajTotal - 1 : 0;
            if (!animRange.empty()) {
                const auto colon = animRange.find(':');
                auto parseNum = [&](const std::string& s, uint64_t* out) -> bool {
                    try {
                        const long long v = std::stoll(s);
                        *out = v < 0 ? (trajTotal > 0 ? trajTotal - 1 : 0)
                                     : static_cast<uint64_t>(v);
                        return true;
                    } catch (...) {
                        return false;
                    }
                };
                const std::string a = animRange.substr(0, colon);
                const std::string b = colon == std::string::npos ? "" : animRange.substr(colon + 1);
                if (!parseNum(a, &from) || (!b.empty() && !parseNum(b, &to))) {
                    spdlog::error("[cli] render: --anim-range 需为 a:b，收到 '{}'", animRange);
                    std::exit(3);
                }
            }
            if (trajTotal > 0 && to >= trajTotal) {
                to = trajTotal - 1;
            }
            if (from > to) {
                spdlog::error("[cli] render: 动画起始帧 {} > 结束帧 {}", from, to);
                std::exit(3);
            }
            const int stp = std::max(1, animStep);
            const std::string framesDir = animOut + ".frames";
            std::error_code ec;
            std::filesystem::create_directories(framesDir, ec);
            if (ec) {
                spdlog::error("[cli] render: 无法创建帧目录 {}: {}", framesDir, ec.message());
                std::exit(3);
            }
            int written = 0;
            bool diagOnce = false;   // 首次帧打印映射/盒子自检（MOLECULENS_ANIM_DEBUG=1）
            const bool animDebug = std::getenv("MOLECULENS_ANIM_DEBUG") != nullptr;
            std::vector<glm::vec3> dbgPrev;
            const int interp = std::max(1, animInterp);
            auto writeFramePng = [&](const std::vector<glm::vec3>& positions,
                                     const std::array<float, 9>& box) {
                if (animDebug && written < 20) {
                    if (!dbgPrev.empty() && dbgPrev.size() == positions.size()) {
                        double sum = 0.0;
                        float mx = 0.0f;
                        for (size_t i = 0; i < positions.size(); ++i) {
                            const float d = glm::length(positions[i] - dbgPrev[i]);
                            sum += d;
                            mx = std::max(mx, d);
                        }
                        spdlog::warn("[cli] anim-frame {}: meanDisp={:.2f} Å max={:.2f} Å",
                                     written,
                                     sum / static_cast<double>(positions.size()), mx);
                    }
                    dbgPrev = positions;
                }
                applySystemFrameToScene(*parsed->scene, positions, box);
                // 强制整条命令流重建：坐标重放路径在"逐帧换整体系坐标"时会把
                // 网格/键端点写串（实测相邻帧画面像随机重排），导出必须优先正确性。
                parsed->scene->setDirty(entity::DIRTY_INSTANCE);
                // update 消费 DIRTY_COORDS（置 needsCoords_），prepare 才会走"坐标重放"；
                // 只调 prepare 会一直复用首帧命令流（实测 21 帧全同）。
                renderer.update(0.0f);
                renderer.prepare();
                off.execute(renderer.commandList(), view);
                std::vector<uint8_t> px;
                if (!off.readbackPixels(px, engine::PixelFormat::Rgba8)) {
                    spdlog::error("[cli] render: 动画帧像素回读失败");
                    std::exit(3);
                }
                std::vector<uint8_t> finalPx =
                    ss > 1 ? downsample(px, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height),
                                        static_cast<uint32_t>(ss))
                           : px;
                char fname[32];
                std::snprintf(fname, sizeof(fname), "frame_%04d", written);
                const std::string framePath =
                    (std::filesystem::path(framesDir) / (std::string(fname) + ".png")).string();
                if (!engine::writePng(framePath, static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height), finalPx)) {
                    spdlog::error("[cli] render: 动画帧写出失败: {}", framePath);
                    std::exit(3);
                }
                ++written;
            };
            std::vector<glm::vec3> prevCont;   // 上一帧的"连续"坐标（跨帧 PBC 展开后）
            std::vector<glm::vec3> cont;
            for (uint64_t f = from; f <= to; f += static_cast<uint64_t>(stp)) {
                auto ts = rdr->readFrame(f);
                if (!ts) {
                    spdlog::error("[cli] render: 读取轨迹帧 {} 失败: {}", f, ts.errorMessage());
                    std::exit(3);
                }
                // 先做时间轴 PBC 展开，再做帧间插值：顺序反了会在盒子边界处插出
                // "横穿整个盒子的长棍"（分子在相邻帧被 wrap 到对面）。
                // 时间轴展开 → 连续缓冲；插值必须在这个连续空间里做，
                // 否则跨盒分子会在中间帧被拉成横穿盒子的长棍。
                const std::vector<glm::vec3> prevSnapshot = prevCont;
                unwrapAcrossTime(*parsed->scene, ts->positions, ts->box, &prevCont, &cont);
                if (!prevSnapshot.empty() && prevSnapshot.size() == cont.size() &&
                    std::getenv("MOLECULENS_ANIM_DEBUG") != nullptr) {
                    double sum = 0.0;
                    float mx = 0.0f;
                    for (size_t i = 0; i < cont.size(); ++i) {
                        const float d = glm::length(cont[i] - prevSnapshot[i]);
                        sum += d;
                        mx = std::max(mx, d);
                    }
                    spdlog::warn("[cli] anim-disp frame {}: mean={:.2f} Å max={:.2f} Å",
                                 f, sum / static_cast<double>(cont.size()), mx);
                }
                if (!diagOnce && std::getenv("MOLECULENS_ANIM_DEBUG") != nullptr) {
                    diagOnce = true;
                    spdlog::warn("[cli] anim-diag: atoms={} instances={} boxIndices=({},{},{}) boxDiag=({:.3f},{:.3f},{:.3f}) replay={}",
                                 ts->positions.size(), parsed->scene->instances().size(),
                                 ts->box[0], ts->box[4], ts->box[8],
                                 ts->box[0], ts->box[4], ts->box[8],
                                 renderer.lastPrepareWasReplay());
                }
                // 帧间线性插值：MD 帧距 10 ps 时直接 30 fps 播放会有明显跳动，
                // 插入 K-1 个中间构象可以让肉眼看到连续运动（不改变导出帧率）。
                if (!prevSnapshot.empty() && interp > 1 &&
                    prevSnapshot.size() == cont.size()) {
                    std::vector<glm::vec3> mid(cont.size());
                    for (int s = 1; s < interp; ++s) {
                        const float t = static_cast<float>(s) / static_cast<float>(interp);
                        for (size_t i = 0; i < cont.size(); ++i) {
                            mid[i] = prevSnapshot[i] + (cont[i] - prevSnapshot[i]) * t;
                        }
                        writeFramePng(mid, ts->box);
                    }
                }
                writeFramePng(cont, ts->box);
            }
            engine::AnimEncodeOptions enc;
            enc.framePattern = (std::filesystem::path(framesDir) / "frame_%04d.png").string();
            enc.outPath = animOut;
            enc.format = engine::animFormatFromPath(animOut);
            enc.fps = animFps > 0 ? animFps : 30;
            std::string encErr;
            if (!engine::encodeAnimation(enc, &encErr)) {
                spdlog::error("[cli] render: 动画编码失败: {}", encErr);
                std::exit(3);
            }
            std::filesystem::remove_all(framesDir, ec);
            spdlog::info("[cli] render: 动画 {} 帧 -> {}（{} fps, {}）", written, enc.outPath,
                         enc.fps, engine::animFormatName(enc.format));
            return;
        }

        // 16-bit：优先走 16F 半浮点附件（线性、不经 8-bit 量化；回读后由
        // writePng16FromHalfRgba 做色调映射 + sRGB + 16-bit 量化）。
        // 设备不支持时退回下方的 8-bit 多采样求平均路径。
        if (depth == 16 &&
            off.setColorAttachment(engine::ColorAttachment::Rgba16Float)) {
            off.execute(renderer.commandList(), view);
            std::vector<uint8_t> halfBytes;
            if (!off.readbackPixels(halfBytes, engine::PixelFormat::Rgba16F)) {
                spdlog::error("[cli] render: 16F 像素回读失败");
                std::exit(3);
            }
            std::vector<uint16_t> half(halfBytes.size() / 2);
            if (!halfBytes.empty()) {
                std::memcpy(half.data(), halfBytes.data(), halfBytes.size());
            }
            if (!engine::writePng16FromHalfRgba(out, static_cast<uint32_t>(width),
                                                static_cast<uint32_t>(height), half, rw,
                                                rh)) {
                spdlog::error("[cli] render: PNG(16-bit/HDR) 写出失败: {}", out);
                std::exit(3);
            }
            spdlog::info("[cli] render: {} -> {} ({}x{}, ssaa={}, 16-bit via 16F)",
                         input, out, width, height, ss);
            return;
        }
        if (depth == 16) {
            spdlog::warn("[cli] render: 16F 颜色附件不可用 → 退回 8-bit 多采样求平均");
        }
        off.execute(renderer.commandList(), view);

        std::vector<uint8_t> px;
        if (!off.readbackPixels(px, engine::PixelFormat::Rgba8)) {
            spdlog::error("[cli] render: 像素回读失败");
            std::exit(3);
        }

        // —— 4. SSAA 降采样 + 编码 ——
        // 16-bit：先在浮点上对 ssaa² 个样本求平均，再量化到 16 bit —— 这是 16-bit
        // 唯一**真实**有意义的来源（N 个样本的均值携带超过 8 bit 的信息量，
        // 量化回 8 bit 会丢弃它；ssaa=1 时 16-bit 只是容器变宽，无额外精度）。
        if (depth == 16) {
            std::vector<uint16_t> out16(static_cast<size_t>(width) * height * 4u);
            const uint32_t n = static_cast<uint32_t>(ss) * ss;
            const float inv = 1.0f / static_cast<float>(n);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    for (int dy = 0; dy < ss; ++dy) {
                        for (int dx = 0; dx < ss; ++dx) {
                            const size_t s = (static_cast<size_t>(y * ss + dy) * rw +
                                              static_cast<size_t>(x * ss + dx)) * 4u;
                            for (int c = 0; c < 4; ++c) {
                                acc[c] += static_cast<float>(px[s + c]);
                            }
                        }
                    }
                    const size_t d = (static_cast<size_t>(y) * width + x) * 4u;
                    for (int c = 0; c < 4; ++c) {
                        // 8-bit 区间 [0,255] → 16-bit [0,65535]：乘 257
                        const float v = acc[c] * inv * 257.0f;
                        out16[d + c] =
                            static_cast<uint16_t>(std::clamp(v, 0.0f, 65535.0f) + 0.5f);
                    }
                }
            }
            if (!engine::writePng16(out, static_cast<uint32_t>(width),
                                    static_cast<uint32_t>(height), out16)) {
                spdlog::error("[cli] render: PNG(16-bit) 写出失败: {}", out);
                std::exit(3);
            }
            spdlog::info("[cli] render: {} -> {} ({}x{}, ssaa={}, 16-bit)", input, out,
                         width, height, ss);
            return;
        }
        std::vector<uint8_t> finalPx =
            ss > 1 ? downsample(px, static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), static_cast<uint32_t>(ss))
                   : px;
        // ESP 叠加：色标条 + 极值点数值（与 GUI 视口叠加信息同源；无 ESP 表面时为空）
        if (std::getenv("MOLECULENS_NO_ESP_OVERLAY") == nullptr) {
            const engine::EspOverlay ov = engine::espOverlaySnapshot();
            if (ov.active) {
                const glm::mat4 vp = cam.viewProjMatrix(
                    static_cast<float>(width) / static_cast<float>(height));
                engine::esp_overlay::drawEspOverlay(finalPx, width, height, vp, ov,
                                                    /*darkBackground=*/bg != "white");
            }
        }
        if (!engine::writePng(out, static_cast<uint32_t>(width),
                              static_cast<uint32_t>(height), finalPx)) {
            spdlog::error("[cli] render: PNG 写出失败: {}", out);
            std::exit(3);
        }
        spdlog::info("[cli] render: {} -> {} ({}x{}, ssaa={})", input, out, width, height,
                     ss);
    });
    return 0;
}

} // namespace moleculens::cli
