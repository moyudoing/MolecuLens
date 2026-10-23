// =============================================================================
// MolecuLens v2 · app/workbench/modules.cpp —— 9 个检查器模块
//
// 检查器是 v2 的"参数面板"：每个模块只负责**一个关注点**的控件布局，
// 状态一律经控制器改（这里不直接碰 Renderer / SceneStructure 的可变状态）。
// 模块通过 IInspectorModule 注册进 Workbench，左导航栏按注册顺序排列，
// 新增模块无需改动布局代码 —— 这就是 v2 "模块化"在 UI 层的落点。
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "app/controller/camera/camera_controller.h"
#include "app/controller/io/io_controller.h"
#include "app/controller/rep/representation_controller.h"
#include "app/controller/render/render_controller.h"
#include "app/controller/scene/scene_controller.h"
#include "app/controller/timeline/timeline_controller.h"
#include "app/data/data_manager.h"
#include "app/i18n/language.h"
#include "app/view/viewport/mol_scene.h"
#include "app/workbench/analysis/molecule_stats.h"
#include "app/workbench/analysis/xlsx_writer.h"
#include "app/workbench/style/aurora.h"
#include "app/workbench/workbench.h"
#include "engine/render/render_parameter.h"
#include "engine/style/espsurface/esp_surface.h"
#include "entity/scene/scene.h"
#include "entity/selection/parse_table.h"
#include "entity/selection/selection.h"
#include "io/cube/cube_volume.h"
#include "persist/settings/settings.h"

namespace moleculens::app {
namespace {

using engine::ColorScheme;
using engine::Representation;
using engine::RepresentationType;

// 主题强调色（Settings::accent，格式 "#RRGGBB"）。v1 的 "teal" 等旧值无法解析
// → 回落到内置蓝；clearAccent 时写空串。
std::string hexColorOf(const ImVec4& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<int>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f + 0.5f));
    return buf;
}

void persistAccent(const ImVec4* color) {
    persist::Settings s;
    auto loaded = persist::SettingsStore::load();
    if (loaded) {
        s = *loaded;
    }
    s.accent = color != nullptr ? hexColorOf(*color) : std::string();
    (void)persist::SettingsStore::save(s);
}

// —— 绘图样式（与 RepresentationType 一一对应；顺序即下拉顺序）——
struct TypeEntry {
    RepresentationType type;
    const char* label;
};
const TypeEntry kTypes[] = {
    {RepresentationType::Wireframe, "线框"},
    {RepresentationType::Stick, "棍状"},
    {RepresentationType::BallAndStick, "球棍"},
    {RepresentationType::Spacefill, "空间填充"},
    {RepresentationType::Licorice, "甘草棍"},
    {RepresentationType::Vdw, "范德华球"},
    {RepresentationType::QuickSurf, "快速表面"},
    {RepresentationType::Surface, "分子表面"},
    {RepresentationType::EspSurface, "ESP 映射表面"},
    {RepresentationType::Tube, "管状"},
    {RepresentationType::Cartoon, "卡通"},
    {RepresentationType::NewCartoon, "改进卡通"},
    {RepresentationType::Ribbon, "丝带"},
    {RepresentationType::Plate, "平板（辅基）"},
    {RepresentationType::Diamond, "菱形（非氢原子）"},
    {RepresentationType::DotCloud, "点云"},
    {RepresentationType::Polyhedron, "多面体（非氢原子）"},
};
constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));

const char* typeLabel(RepresentationType t) {
    for (const auto& e : kTypes) {
        if (e.type == t) {
            return e.label;
        }
    }
    return "球棍";
}

int typeIndex(RepresentationType t) {
    for (int i = 0; i < kTypeCount; ++i) {
        if (kTypes[i].type == t) {
            return i;
        }
    }
    return 2;
}

const char* kColorSchemeNames[] = {"元素 CPK", "按链", "按残基", "按 BFactor",
                                   "统一色", "按原子名"};

void applyMaterial(RepresentationController* ctl, int32_t id, int presetIdx) {
    if (ctl == nullptr || presetIdx < 0 || presetIdx >= engine::kMaterialPresetCount) {
        return;
    }
    const engine::MaterialPreset& p = engine::materialPresetAt(presetIdx);
    const float ambient[4] = {p.ambient[0], p.ambient[1], p.ambient[2], 1.0f};
    const float diffuse[4] = {p.diffuse[0], p.diffuse[1], p.diffuse[2], 1.0f};
    const float specular[4] = {p.specular[0], p.specular[1], p.specular[2], 1.0f};
    ctl->setMaterial(id, presetIdx, ambient, diffuse, specular, p.shininess,
                     p.metallic, p.roughness, p.opacity);
}

void setOpacity(RepresentationController* ctl, int32_t id, const Representation& r,
                float opacity) {
    if (ctl == nullptr) {
        return;
    }
    const float ambient[4] = {r.ambient[0], r.ambient[1], r.ambient[2], 1.0f};
    const float diffuse[4] = {r.diffuse[0], r.diffuse[1], r.diffuse[2], 1.0f};
    const float specular[4] = {r.specular[0], r.specular[1], r.specular[2], 1.0f};
    ctl->setMaterial(id, r.materialPreset, ambient, diffuse, specular, r.shininess,
                     r.metallic, r.roughness, opacity);
}

// 分子列表按结构聚合后的行名："SOL #2032" → "SOL"，"mol# 7" → "mol"
// （解析器把「残基名 + 实例序号」写进实例名，聚合后序号没有意义）
std::string groupBaseName(const std::string& name) {
    size_t cut = name.find(" #");
    if (cut == std::string::npos) {
        cut = name.find('#');
    }
    if (cut == std::string::npos) {
        return name;
    }
    std::string base = name.substr(0, cut);
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    return base;
}

// —— v2.9：给单个表示按结构（resname）挑分子 ——
// 例：表示 A = 全部分子的 QuickSurf，表示 B = 只给非溶剂分子画球棍（selection = "not resname SOL"）。
struct RepMolGroup {
    std::string name;   // resname，如 "SOL"
    size_t instances = 0;
    size_t atoms = 0;
};

std::vector<RepMolGroup> collectRepMolGroups(DataManager* dm) {
    std::vector<RepMolGroup> groups;
    if (dm == nullptr) {
        return groups;
    }
    const size_t n = dm->moleculeCount();
    for (size_t i = 0; i < n; ++i) {
        const std::string base = groupBaseName(dm->moleculeName(i));
        if (base.empty()) {
            continue;
        }
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const RepMolGroup& g) { return g.name == base; });
        if (it == groups.end()) {
            groups.push_back(RepMolGroup{base, 1, dm->atomCount(i)});
        } else {
            ++it->instances;
            it->atoms += dm->atomCount(i);
        }
    }
    return groups;
}

struct ResnameSelection {
    bool valid = true;          // UI 能理解的 resname 组合（其它表达式不接管）
    bool defaultInclude = true; // 未列出的类型是否包含（"none" = false）
    // 类型名 → 是否包含。支持 "not resname X and not resname Y"、
    // "not (resname X or resname Y)"、"resname A or resname B" 以及旧版的
    // "resname A B C"（多名字）。
    std::vector<std::pair<std::string, bool>> flags;
};

ResnameSelection parseResnameSelection(const std::string& sel) {
    ResnameSelection out;
    std::vector<std::string> toks;
    std::string cur;
    for (const char c : sel) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        toks.push_back(cur);
    }
    std::vector<std::string> flat;   // 去掉括号：本 UI 只关心 resname 的包含/排除
    for (const std::string& t : toks) {
        if (t != "(" && t != ")") {
            flat.push_back(t);
        }
    }
    if (flat.empty()) {
        return out;   // 空表达式 = 全部
    }
    const auto lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };
    if (flat.size() == 1 && (lower(flat[0]) == "none")) {
        out.defaultInclude = false;
        return out;
    }
    if (flat.size() == 1 && (lower(flat[0]) == "all")) {
        return out;
    }
    size_t i = 0;
    bool sawResname = false;
    while (i < flat.size()) {
        bool neg = false;
        if (lower(flat[i]) == "not") {
            neg = true;
            ++i;
        }
        if (i >= flat.size() || lower(flat[i]) != "resname") {
            out.valid = false;   // within / 链/残基等复杂表达式：UI 不接管
            return out;
        }
        ++i;
        sawResname = true;
        bool any = false;
        while (i < flat.size()) {
            const std::string t = lower(flat[i]);
            if (t == "and" || t == "or") {
                ++i;
                break;
            }
            if (t == "not" || t == "resname") {
                break;   // 宽容：没有连接词的相邻子句
            }
            out.flags.emplace_back(flat[i], !neg);
            any = true;
            ++i;
        }
        if (!any) {
            out.valid = false;
            return out;
        }
    }
    if (!sawResname) {
        out.valid = false;
    }
    return out;
}

bool selectionIncludesGroup(const std::string& sel, const std::string& name) {
    const ResnameSelection ps = parseResnameSelection(sel);
    if (!ps.valid) {
        return true;   // 复杂表达式交给引擎，UI 不显示勾选状态（默认勾）
    }
    for (const auto& kv : ps.flags) {
        if (kv.first == name) {
            return kv.second;
        }
    }
    return ps.defaultInclude;
}

std::string buildResnameSelection(const std::vector<std::string>& included,
                                  const std::vector<std::string>& all) {
    if (included.empty()) {
        return "none";
    }
    if (included.size() >= all.size()) {
        return std::string();
    }
    std::vector<std::string> excluded;
    for (const auto& a : all) {
        if (std::find(included.begin(), included.end(), a) == included.end()) {
            excluded.push_back(a);
        }
    }
    std::string out;
    if (included.size() <= excluded.size()) {
        // 选择引擎每个比较只接受一个值：多个 resname 必须写成 "resname A or resname B"。
        bool first = true;
        for (const auto& n : included) {
            if (!first) {
                out += " or ";
            }
            out += "resname ";
            out += n;
            first = false;
        }
    } else {
        // 排除项少：用 "not (resname X or resname Y)"，避免旧版 "not resname X Y Z"
        // 只生效第一个名字（引擎会忽略多余 token）的静默 bug。
        out = "not (";
        bool first = true;
        for (const auto& n : excluded) {
            if (!first) {
                out += " or ";
            }
            out += "resname ";
            out += n;
            first = false;
        }
        out += ")";
    }
    return out;
}

// 一键风格：「Glass + DynamicBonds」观感（v2.6）
//   表面 = 整体系 QuickSurf + Glass2 + 不透明度 0.32 + Name 配色
//   球棍 = Opaque + Name 配色（在玻璃壳内部，透过壳可见）
//   背景 = 近白（棚拍观感）
[[maybe_unused]] void applyGlassPreset(Workbench& wb) {
    RepresentationController* ctl = wb.reps();
    if (ctl == nullptr) {
        return;
    }
    int32_t surfId = -1;
    int32_t ballId = -1;
    const auto& reps = ctl->representations();
    for (size_t i = 0; i < reps.size(); ++i) {
        const RepresentationType t = reps[i].type;
        if (t == RepresentationType::QuickSurf || t == RepresentationType::Surface) {
            surfId = static_cast<int32_t>(i);
        } else if (t == RepresentationType::BallAndStick) {
            ballId = static_cast<int32_t>(i);
        }
    }
    if (surfId < 0) {
        surfId = ctl->addRepresentation("玻璃表面", RepresentationType::QuickSurf,
                                        std::string(), ColorScheme::AtomName);
    }
    if (ballId < 0) {
        ballId = ctl->addRepresentation("球棍", RepresentationType::BallAndStick,
                                        std::string(), ColorScheme::AtomName);
    }
    const int glass = engine::materialPresetIndexByName("Glass2");
    if (surfId >= 0) {
        ctl->setColorScheme(surfId, ColorScheme::AtomName);
        ctl->setSystemSurface(surfId, true);
        applyMaterial(ctl, surfId, glass >= 0 ? glass : 0);
        const auto& sr = ctl->representations()[static_cast<size_t>(surfId)];
        setOpacity(ctl, surfId, sr, 0.32f);
    }
    if (ballId >= 0) {
        ctl->setColorScheme(ballId, ColorScheme::AtomName);
        applyMaterial(ctl, ballId, 0);
        const auto& br = ctl->representations()[static_cast<size_t>(ballId)];
        setOpacity(ctl, ballId, br, 1.0f);
    }
    if (auto* rc = wb.renderCtl(); rc != nullptr) {
        rc->setBackgroundColor(0.98f, 0.98f, 0.98f);
    }
    wb.setActiveRepIndex(surfId >= 0 ? surfId : ballId);
}

// 反向：所有表示回到不透明默认材质（表面仍保留，只是不再透）
[[maybe_unused]] void applyOpaquePreset(Workbench& wb) {
    RepresentationController* ctl = wb.reps();
    if (ctl == nullptr) {
        return;
    }
    const auto& reps = ctl->representations();
    for (size_t i = 0; i < reps.size(); ++i) {
        const int32_t id = static_cast<int32_t>(i);
        applyMaterial(ctl, id, 0);
        const auto& r = ctl->representations()[i];
        setOpacity(ctl, id, r, 1.0f);
    }
}

// —— 样式适用性（选择器防"选了变空白"）——
// 卡通/管状/丝带等骨架样式只对**蛋白质**（有 CA 原子/残基链）有意义。此前下拉
// 无条件列出全部 14 项：在水盒/小分子上选中即得到一张白屏，用户完全无法理解
// 发生了什么（"显示视图的选择器 bug"）。这里做两件事：
//   1) 无可渲染骨架时把骨架样式置灰并给出原因；
//   2) 当前表示已是骨架样式却无骨架时，在面板上直接提示 + 一键改回球棍。
bool isBackboneOnlyStyle(RepresentationType type) {
    return type == RepresentationType::Cartoon || type == RepresentationType::Tube ||
           type == RepresentationType::NewCartoon || type == RepresentationType::Ribbon;
}

bool sceneHasProteinBackbone(const Workbench& wb) {
    const entity::SceneStructure* sc = wb.scene();
    if (sc == nullptr) {
        return false;
    }
    // 以 SceneStructure::serial() 缓存：表里每帧都可能查询，6452 原子逐个查名字
    // 没有必要。serial 在结构/实例/表示变更时递增，缓存失效口径足够。
    static const entity::SceneStructure* cachedScene = nullptr;
    static uint64_t cachedSerial = UINT64_MAX;
    static bool cached = false;
    const uint64_t serial = sc->serial();
    if (cachedScene == sc && cachedSerial == serial) {
        return cached;
    }
    cachedScene = sc;
    cachedSerial = serial;
    cached = false;
    for (size_t si = 0; si < sc->structures().size() && !cached; ++si) {
        const auto& st = sc->structures()[si];
        int caCount = 0;
        for (size_t ai = 0; ai < st.atoms.size(); ++ai) {
            if (st.atoms[ai].residueId < 0) {
                continue;
            }
            const std::string_view nm = sc->atomName(static_cast<uint32_t>(si),
                                                     static_cast<uint32_t>(ai));
            if (nm == "CA" || nm == "CA " || nm == " C A") {
                if (++caCount >= 6) {
                    cached = true;
                    break;
                }
            }
        }
    }
    return cached;
}

std::string repLabel(const std::vector<Representation>& reps, size_t i) {
    char buf[160];
    const Representation& r = reps[i];
    std::snprintf(buf, sizeof(buf), "%s  ·  %s", tr(typeLabel(r.type)),
                  r.name.empty() ? tr("未命名") : r.name.c_str());
    return buf;
}

int32_t activeId(Workbench& wb) {
    const auto& reps = wb.representations();
    if (reps.empty()) {
        return -1;
    }
    return std::clamp<int32_t>(wb.activeRepIndex(), 0,
                               static_cast<int32_t>(reps.size()) - 1);
}

// 下拉：选择"正在编辑的表示"
void repPicker(Workbench& wb) {
    const auto& reps = wb.representations();
    if (reps.empty()) {
        return;
    }
    const int32_t cur = activeId(wb);
    const std::string preview = repLabel(reps, static_cast<size_t>(cur));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##rep_pick", preview.c_str())) {
        for (int32_t i = 0; i < static_cast<int32_t>(reps.size()); ++i) {
            ImGui::PushID(i);
            const std::string label = repLabel(reps, static_cast<size_t>(i));
            if (ImGui::Selectable(label.c_str(), i == cur)) {
                wb.setActiveRepIndex(i);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

// 样式下拉（写回控制器）
[[maybe_unused]] void typeCombo(Workbench& wb, int32_t id, const Representation& r) {
    std::string items;
    for (const auto& e : kTypes) {
        items += tr(e.label);
        items.push_back('\0');
    }
    int idx = typeIndex(r.type);
    aurora::sectionLabel(tr("绘图样式"));
    if (ImGui::Combo("##type", &idx, items.c_str())) {
        wb.reps()->setType(id, kTypes[idx].type);
    }
}

[[maybe_unused]] void colorCombo(Workbench& wb, int32_t id, const Representation& r) {
    int idx = std::clamp<int>(static_cast<int>(r.colorScheme), 0,
                              static_cast<int>(ColorScheme::Count) - 1);
    std::string items;
    for (const char* n : kColorSchemeNames) {
        items += tr(n);
        items.push_back('\0');
    }
    aurora::sectionLabel(tr("配色"));
    if (ImGui::Combo("##color", &idx, items.c_str())) {
        wb.reps()->setColorScheme(id, static_cast<ColorScheme>(idx));
    }
}

// 材质预设的中文显示名（引擎层仍以英文名作为稳定键/CLI 参数；此处只影响 UI）。
// 与 kMaterialPresets 一一对应，数量由 static_assert 守住，增删预设会在此报错。
const char* materialPresetNameZh(int index) {
    static const char* const kZh[] = {
        "不透明", "透明", "拉丝金属", "漫反射", "幽灵",
        "玻璃 1", "玻璃 2", "玻璃 3", "光泽", "硬塑料",
        "金属粉彩", "钢", "半透明", "硬边", "硬边高光",
        "硬边玻璃", "Goodsell 风格", "AO 高光", "AO 哑光", "AO 硬边",
        "吹制玻璃", "玻璃气泡", "光追铬",
    };
    static_assert(sizeof(kZh) / sizeof(kZh[0]) ==
                      static_cast<size_t>(engine::kMaterialPresetCount),
                  "materialPresetNameZh 与 kMaterialPresets 数量不一致");
    if (index < 0 || index >= engine::kMaterialPresetCount) {
        return tr("自定义");
    }
    return tr(kZh[index]);
}

void materialCombo(Workbench& wb, int32_t id, const Representation& r) {
    int idx = r.materialPreset;
    if (idx < 0 || idx >= engine::kMaterialPresetCount) {
        idx = engine::kMaterialPresetCount;   // 自定义
    }
    std::string items;
    for (int i = 0; i < engine::kMaterialPresetCount; ++i) {
        items += materialPresetNameZh(i);
        items.push_back('\0');
    }
    items += tr("自定义");
    items.push_back('\0');
    aurora::sectionLabel(tr("材质"));
    if (ImGui::Combo("##material", &idx, items.c_str())) {
        if (idx < engine::kMaterialPresetCount) {
            applyMaterial(wb.reps(), id, idx);
        }
    }
}

// 统一色：颜色写材质 diffuse；环境色取 55% 同色，保证暗部也带同色相。
void applyUniformColor(RepresentationController* ctl, int32_t id,
                       const Representation& r, const float c[3]) {
    if (ctl == nullptr) {
        return;
    }
    const float ambient[4] = {c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f, 1.0f};
    const float diffuse[4] = {c[0], c[1], c[2], 1.0f};
    const float specular[4] = {r.specular[0], r.specular[1], r.specular[2], 1.0f};
    ctl->setMaterial(id, r.materialPreset, ambient, diffuse, specular, r.shininess,
                     r.metallic, r.roughness, r.opacity);
}

// =============================================================================
// 1. 显示（Visualize）—— 样式 / 材质 / 选择表达式 / 表面（v2.7 合并）
// =============================================================================
// 定义在下面「表面」段；VisualizeModule 里以折叠区调用
void renderSurfaceSection(Workbench& wb);

// 渲染质量（分辨率 / 动态分辨率 / 网格面数）。放在「渲染视图」顶部，
// 避免被长表单压到折叠区以下；导出分辨率不受这里影响。
void renderRenderQuality(Workbench& wb) {
    if (wb.viewport() == nullptr) {
        return;
    }
    aurora::sectionLabel(tr("渲染质量"), tr("影响流畅度与清晰度"));
    float rs = wb.viewport()->renderScale();
    if (aurora::sliderRow(tr("渲染分辨率"), &rs, 0.50f, 2.00f, "%.2fx",
                          tr("离屏渲染分辨率相对视口的比例；降低可明显提升"
                             "大体系播放帧率"))) {
        wb.viewport()->setRenderScale(rs);
    }
    bool dyn = wb.viewport()->dynamicResolution();
    if (aurora::toggleRow(tr("动态分辨率"), &dyn,
                          tr("拖动/播放时自动降到 0.7×，停手 350 ms 后恢复；"
                             "只影响视口，不影响导出"))) {
        wb.viewport()->setDynamicResolution(dyn);
    }
    static const char* kMeshItems = "低 (80 面)\0中 (320 面)\0高 (1280 面)\0"
                                    "极高 (5120 面)\0";
    int meshIdx = wb.viewport()->meshDetail() - 1;
    if (aurora::comboRow(tr("网格面数"), &meshIdx, kMeshItems,
                         tr("球体细分档；面数越高越圆润，也越吃 GPU"))) {
        static const int kDetail[4] = {1, 2, 3, 4};
        static const int kSegments[4] = {8, 12, 24, 48};
        const int i = std::clamp(meshIdx, 0, 3);
        wb.viewport()->setMeshQuality(kDetail[i], kSegments[i]);
    }
    ImGui::TextDisabled("%s", tr("光追：仅导出图片时使用（CPU 回退）"));
    aurora::spacer(4.0f);
}

// v2.10：分子列表按「体系」分组（体系 = 一次导入；导入时各自居中并横向错开）。
// 体系内再按结构聚合：同构实例（如 2032 个 SOL）只占一行，避免几千行同名条目；
// 分组也避免两个文件里同名的 SOL 被合并成同一行。
static void renderMoleculeSection(Workbench& wb) {
    DataManager* dm = wb.data();
    SceneController* ctl = wb.sceneCtl();
    if (dm == nullptr || ctl == nullptr) {
        ImGui::TextDisabled("%s", tr("数据未就绪"));
        return;
    }
    const size_t n = dm->moleculeCount();
    if (n == 0) {
        ImGui::TextDisabled("%s", tr("场景为空"));
        return;
    }
    const size_t sysCount = dm->systemCount();
    {
        char sum[160];
        std::snprintf(sum, sizeof(sum), "%s %zu %s · %zu %s", tr("共"), n, tr("个实例"), sysCount,
                      tr("个体系"));
        ImGui::TextDisabled("%s", sum);
    }
    struct MolGroup {
        uint32_t structureIndex = 0;
        std::vector<uint32_t> instances;
    };
    // 多出来的最后一组放「未归组」实例（老场景或手工移动后可能出现）
    for (size_t sys = 0; sys <= sysCount; ++sys) {
        const bool orphan = (sys == sysCount);
        std::vector<uint32_t> members;
        if (orphan) {
            for (size_t i = 0; i < n; ++i) {
                if (dm->systemIdOfInstance(i) == UINT32_MAX) {
                    members.push_back(static_cast<uint32_t>(i));
                }
            }
            if (members.empty()) {
                continue;
            }
        } else {
            const size_t mc = dm->systemMemberCount(sys);
            for (size_t k = 0; k < mc; ++k) {
                members.push_back(dm->systemMemberInstance(sys, k));
            }
        }
        std::vector<MolGroup> groups;
        size_t atoms = 0;
        for (uint32_t inst : members) {
            atoms += dm->atomCount(inst);
            const uint32_t sidx = dm->moleculeStructureIndex(inst);
            auto it = std::find_if(groups.begin(), groups.end(), [sidx](const MolGroup& g) {
                return g.structureIndex == sidx;
            });
            if (it == groups.end()) {
                MolGroup g;
                g.structureIndex = sidx;
                g.instances.push_back(inst);
                groups.push_back(std::move(g));
            } else {
                it->instances.push_back(inst);
            }
        }
        ImGui::PushID(3000 + static_cast<int>(sys));
        if (!orphan) {
            bool sysVis = dm->systemVisible(sys);
            if (ImGui::Checkbox("##sysvis", &sysVis)) {
                ctl->setSystemVisible(dm->systemId(sys), sysVis);
            }
            ImGui::SameLine();
        }
        const std::string sname = orphan ? std::string(tr("未归组")) : dm->systemName(sys);
        const bool open = ImGui::TreeNodeEx(
            (sname + "##sys" + std::to_string(sys)).c_str(),
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::SameLine();
        char info[128];
        std::snprintf(info, sizeof(info), "(%zu %s · %zu %s)", members.size(), tr("实例"), atoms,
                      tr("原子"));
        ImGui::TextDisabled("%s", info);
        ImGui::SameLine();
        if (!orphan && ImGui::SmallButton(tr("删除"))) {
            ctl->removeSystem(dm->systemId(sys));
            ImGui::PopID();
            break;
        }
        if (!open) {
            ImGui::PopID();
            continue;
        }
        // 体系独立渲染模式（实心/线框/透明/点）。此前只在 v1 的分子列表里有入口，
        // v2 工作台完全看不到这个功能 —— 后端的 RenderMode 命令其实一直有效。
        if (!orphan) {
            entity::SceneStructure* sc = wb.scene();
            const uint32_t sysId = dm->systemId(sys);
            const uint32_t si = sc != nullptr ? sc->systemIndexById(sysId) : UINT32_MAX;
            if (sc != nullptr && si != UINT32_MAX) {
                int mode = static_cast<int>(sc->systems()[si].renderMode);
                const char* items[] = {tr("实心"), tr("线框"), tr("透明"), tr("点")};
                ImGui::TextDisabled("%s", tr("渲染模式"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                if (aurora::segmented("##sysmode", items, 4, &mode)) {
                    ctl->setSystemRenderMode(sysId, mode);
                }
                aurora::spacer(2.0f);
            }
        }
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const MolGroup& g = groups[gi];
            const uint32_t head = g.instances.front();
            ImGui::PushID(static_cast<int>(gi));
            bool vis = dm->moleculeVisible(head);
            if (ImGui::Checkbox("##vis", &vis)) {
                for (uint32_t inst : g.instances) {
                    ctl->setVisible(inst, vis);
                }
            }
            ImGui::SameLine();
            const std::string name = groupBaseName(dm->moleculeName(head));
            const int32_t cur = wb.selectedMolecule();
            const bool isSel =
                cur >= 0 &&
                std::find(g.instances.begin(), g.instances.end(), static_cast<uint32_t>(cur)) !=
                    g.instances.end();
            if (ImGui::Selectable(name.empty() ? tr("未命名") : name.c_str(), isSel,
                                  ImGuiSelectableFlags_AllowOverlap, ImVec2(140.0f, 0.0f))) {
                wb.setSelectedMolecule(isSel ? -1 : static_cast<int32_t>(head));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s", tr("设为当前分子：新建表示时用它预填选择表达式"),
                                  tr("再次点击可取消"));
            }
            ImGui::SameLine();
            char cinfo[96];
            if (g.instances.size() > 1) {
                std::snprintf(cinfo, sizeof(cinfo), "×%zu (%zu)", g.instances.size(),
                              dm->atomCount(head));
            } else {
                std::snprintf(cinfo, sizeof(cinfo), "(%zu)", dm->atomCount(head));
            }
            ImGui::TextDisabled("%s", cinfo);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("删除"))) {
                std::vector<uint32_t> victims = g.instances;
                std::sort(victims.rbegin(), victims.rend());
                for (uint32_t v : victims) {
                    ctl->removeInstance(v);
                }
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
        ImGui::PopID();
    }
}
// v2.9：表示表格（图形表示对话框的对照布局）。
// 一屏列出「名称 / 样式 / 配色 / 选择」，点行切换当前编辑对象，行尾 × 直接删除；
// 比「下拉选一个再编辑」更适合 3 条以上表示的场合。
// =============================================================================
// 「作用分子」编辑器（v2.19）
// 表格「选择」列弹层与显示面板的折叠区共用。这里有两块**正交**的选择：
//   · 类型勾选 → 改写 resname 表达式（原子级 Selection，引擎按原子过滤）；
//   · 类型范围 → 按分子**实例**过滤（比例 / 步长，引擎在 prepare 里消费）。
// 两者取交集；"只渲染一半溶剂"这类需求用表达式做不到（resname 是全有/全无），
// 所以范围必须落在实例层，且随 .mlscene 持久化。
// =============================================================================
static void renderMolScopeEditor(Workbench& wb, int32_t repId, const std::string& curSel) {
    DataManager* dm = wb.data();
    RepresentationController* ctl = wb.reps();
    if (dm == nullptr || ctl == nullptr) {
        ImGui::TextDisabled("%s", tr("数据未就绪"));
        return;
    }
    const auto& reps = ctl->representations();
    if (repId < 0 || static_cast<size_t>(repId) >= reps.size()) {
        ImGui::TextDisabled("%s", tr("表示不存在"));
        return;
    }
    const Representation rep = reps[static_cast<size_t>(repId)];   // 本帧快照
    const std::vector<RepMolGroup> groups = collectRepMolGroups(dm);
    if (groups.empty()) {
        ImGui::TextDisabled("%s", tr("场景为空"));
        return;
    }
    ImGui::PushID(static_cast<int>(repId) + 1000);

    std::vector<std::string> all;
    for (const RepMolGroup& g : groups) {
        all.push_back(g.name);
    }
    auto applyIncluded = [&](const std::vector<std::string>& inc) {
        ctl->setSelection(repId, buildResnameSelection(inc, all));
    };
    auto isSolventName = [](const std::string& n) {
        return n == "SOL" || n == "WAT" || n == "HOH" || n == "TIP3" || n == "TIP3P" || n == "H2O";
    };
    // 只做 ASCII 小写：类型名是 SOL / PROA / HOH 这类符号，统一小写即可让搜索
    // "sol"/"SOL" 都能命中（不引入 <cctype> 与 locale 依赖）。
    auto lowerAscii = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };

    const float bw = std::max((ImGui::GetContentRegionAvail().x - 12.0f) / 3.0f, 52.0f);
    if (ImGui::Button(tr("全部分子"), ImVec2(bw, 0.0f))) {
        applyIncluded(all);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("仅非溶剂"), ImVec2(bw, 0.0f))) {
        std::vector<std::string> inc;
        for (const std::string& n : all) {
            if (!isSolventName(n)) {
                inc.push_back(n);
            }
        }
        applyIncluded(inc);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("清空"), ImVec2(bw, 0.0f))) {
        ctl->setSelection(repId, "none");
    }
    aurora::spacer(2.0f);
    ImGui::TextDisabled("%s", curSel.empty() ? tr("当前：全部分子") : curSel.c_str());

    // 搜索：类型多起来（蛋白 + 溶剂 + 离子）后逐行找 SOL 太费眼。
    // static：两个调用点编辑的是同一个表示编辑器，共用一份搜索词是预期行为。
    static char scopeFilter[64] = {};
    aurora::spacer(2.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##mol_scope_filter", tr("搜索类型（如 SOL）"), scopeFilter,
                             sizeof(scopeFilter));
    const std::string filterLower = lowerAscii(scopeFilter);

    aurora::spacer(2.0f);
    size_t visibleTypes = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const RepMolGroup& g = groups[gi];
        if (!filterLower.empty() &&
            lowerAscii(g.name).find(filterLower) == std::string::npos) {
            continue;
        }
        ++visibleTypes;
        bool on = selectionIncludesGroup(curSel, g.name);
        ImGui::PushID(static_cast<int>(gi));
        const std::string lbl = g.name + "##mg";
        if (ImGui::Checkbox(lbl.c_str(), &on)) {
            std::vector<std::string> inc;
            for (const RepMolGroup& gg : groups) {
                const bool cur =
                    (gg.name == g.name) ? on : selectionIncludesGroup(curSel, gg.name);
                if (cur) {
                    inc.push_back(gg.name);
                }
            }
            applyIncluded(inc);
        }
        ImGui::SameLine();
        char info[128];
        const size_t perInst = g.instances > 0 ? g.atoms / g.instances : 0;
        std::snprintf(info, sizeof(info), "×%zu（%zu 原子/实例 · 共 %zu 原子）",
                      g.instances, perInst, g.atoms);
        ImGui::TextDisabled("%s", info);
        ImGui::PopID();
    }
    if (visibleTypes == 0) {
        ImGui::TextDisabled("%s", tr("没有匹配的类型"));
    }

    // —— 类型范围（实例级，v2.19）——
    aurora::spacer(6.0f);
    aurora::hline();
    aurora::sectionLabel(tr("类型范围"), tr("按类型筛选实例；与上面的原子选择取交集"));

    // 类型下拉：全部类型 + 场景中实际出现的类型；旧文件/手工写的类型也要能回显
    std::vector<std::string> typeChoices;
    typeChoices.emplace_back();   // 0 = 全部类型
    for (const RepMolGroup& g : groups) {
        typeChoices.push_back(g.name);
    }
    if (!rep.rangeType.empty() &&
        std::find(typeChoices.begin() + 1, typeChoices.end(), rep.rangeType) ==
            typeChoices.end()) {
        typeChoices.push_back(rep.rangeType);
    }
    int typeIdx = 0;
    for (size_t i = 1; i < typeChoices.size(); ++i) {
        if (typeChoices[i] == rep.rangeType) {
            typeIdx = static_cast<int>(i);
        }
    }
    std::string typeItems = tr("全部类型");
    typeItems.push_back('\0');
    for (size_t i = 1; i < typeChoices.size(); ++i) {
        typeItems += typeChoices[i];
        typeItems.push_back('\0');
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##range_type", &typeIdx, typeItems.c_str())) {
        ctl->setInstanceRangeType(repId, typeIdx == 0 ? std::string()
                                                      : typeChoices[static_cast<size_t>(typeIdx)]);
    }

    int mode = std::clamp<int>(rep.rangeMode, 0, 2);
    const std::string modeItems = trItems({"全部实例", "按比例", "每隔 N 个"});
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##range_mode", &mode, modeItems.c_str())) {
        ctl->setInstanceRangeMode(repId, static_cast<uint8_t>(mode));
    }

    if (mode == 1) {
        float fracPct = std::clamp(rep.rangeFraction, 0.0f, 1.0f) * 100.0f;
        if (aurora::sliderNum(tr("渲染比例"), &fracPct, 0.0f, 100.0f, "%.0f%%", 1.0f,
                              tr("按该类型内的实例顺序取前 N%（确定性，不随机）"))) {
            ctl->setInstanceRangeFraction(repId, fracPct / 100.0f);
        }
        static const int kQuickPct[] = {25, 50, 75, 100};
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%d%%", kQuickPct[i]);
            const bool active = std::fabs(fracPct - static_cast<float>(kQuickPct[i])) < 0.5f;
            if (aurora::chip(lbl, active)) {
                ctl->setInstanceRangeFraction(repId,
                                              static_cast<float>(kQuickPct[i]) / 100.0f);
            }
        }
    } else if (mode == 2) {
        int stride = static_cast<int>(std::max(1u, rep.rangeStride));
        int offset = static_cast<int>(rep.rangeOffset);
        const bool strideChanged =
            aurora::stepper(tr("每隔 N 个"), &stride, 1, 1, 4096,
                            tr("按该类型内的实例顺序，每 N 个取一个"));
        const bool offsetChanged =
            aurora::stepper(tr("取第 M 个"), &offset, 1, 0, std::max(0, stride - 1),
                            tr("偏移从 0 开始；超出 N-1 会自动折回"));
        if (strideChanged || offsetChanged) {
            ctl->setInstanceRangeStride(repId,
                                        static_cast<uint32_t>(std::max(1, stride)),
                                        static_cast<uint32_t>(std::max(0, offset)));
        }
    }

    // 预览：按当前范围统计"实际会参与渲染的实例/原子"。
    // 走 DataManager 的分子顺序（与场景实例顺序一致），不触发任何渲染/脏位。
    const std::string wantType = rep.rangeType;
    auto typeMatches = [&](size_t i) {
        return wantType.empty() || groupBaseName(dm->moleculeName(i)) == wantType;
    };
    size_t matchInst = 0;
    for (size_t i = 0; i < dm->moleculeCount(); ++i) {
        if (typeMatches(i)) {
            ++matchInst;
        }
    }
    size_t shownInst = 0;
    size_t shownAtoms = 0;
    size_t matchedPos = 0;
    const size_t keepCount =
        mode == 1
            ? static_cast<size_t>(std::ceil(std::clamp(rep.rangeFraction, 0.0f, 1.0f) *
                                            static_cast<double>(matchInst)))
            : matchInst;
    const uint32_t strideNow = std::max(1u, rep.rangeStride);
    const uint32_t offsetNow = rep.rangeOffset % strideNow;
    for (size_t i = 0; i < dm->moleculeCount(); ++i) {
        if (!typeMatches(i)) {
            continue;
        }
        bool keep = true;
        if (mode == 1) {
            keep = matchedPos < keepCount;
        } else if (mode == 2) {
            keep = (static_cast<uint32_t>(matchedPos) % strideNow) == offsetNow;
        }
        if (keep) {
            ++shownInst;
            shownAtoms += dm->atomCount(i);
        }
        ++matchedPos;
    }
    const char* typeLabel = wantType.empty() ? tr("全部类型") : rep.rangeType.c_str();
    if (matchInst == 0) {
        ImGui::TextColored(aurora::palette().warn, "%s", tr("该类型在场景里没有实例"));
    } else if (shownInst >= matchInst) {
        ImGui::Text(tr("将渲染全部 %zu 个 %s 实例（共 %zu 原子）"), shownInst, typeLabel,
                    shownAtoms);
    } else {
        ImGui::Text(tr("将渲染 %zu / %zu 个 %s 实例（共 %zu 原子）"), shownInst, matchInst,
                    typeLabel, shownAtoms);
    }
    aurora::helpTip(
        tr("表达式选原子、类型范围选实例，两者取交集：被范围过滤掉的实例即使表达式命中也不渲染。"));
    ImGui::PopID();
}
// v2.13：表示切换改为**列表**（点击选中、下方表单编辑），不再用表格里的
// 样式/配色/选择单元格弹层。用户明确要求"不使用表格选择渲染类型、作用分子和
// 表达式；选择或新建后在下方编辑"。
static void renderRepList(Workbench& wb) {
    RepresentationController* ctl = wb.reps();
    const auto& reps = wb.representations();
    if (ctl == nullptr || reps.empty()) {
        ImGui::TextDisabled("%s", tr("暂无表示，点下方「新建」添加"));
        return;
    }
    const int32_t active = activeId(wb);
    const float rowH = 26.0f;
    for (size_t i = 0; i < reps.size(); ++i) {
        const Representation rep = reps[i];   // 按值：本帧可能增删
        ImGui::PushID(static_cast<int>(i));
        const bool sel = (static_cast<int32_t>(i) == active);
        // v2.14：选中行统一高饱和蓝底 + 白字，和左侧导航一致；
        // 旧实现把主色压暗后当底色，浅色主题下仍像一块浅灰。
        const auto& pal = aurora::palette();
        const ImVec4 selBg = pal.selection;
        const ImVec4 selEdge = pal.onSelection;
        if (sel) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddRectFilled(
                p0, ImVec2(p0.x + w, p0.y + rowH), aurora::col(selBg), 3.0f);
        }
        bool visible = rep.visible;
        if (ImGui::Checkbox("##vis", &visible)) {
            ctl->setVisible(static_cast<int32_t>(i), visible);
        }
        ImGui::SameLine();
        char line[320];
        std::snprintf(line, sizeof(line), "%s   %s · %s%s",
                      rep.name.empty() ? tr("未命名") : rep.name.c_str(),
                      tr(typeLabel(rep.type)), engine::colorSchemeZh(rep.colorScheme),
                      rep.visible ? "" : tr("   · 隐藏"));
        if (sel || !rep.visible) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  aurora::col(sel ? pal.onSelection : pal.textMuted));
        }
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                              ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (ImGui::Selectable(line, sel, ImGuiSelectableFlags_None,
                              ImVec2(0.0f, rowH))) {
            wb.clearRepMultiSelection();
            wb.setActiveRepIndex(static_cast<int32_t>(i));
        }
        ImGui::PopStyleColor(2);
        if (sel) {
            // 4px 白色竖条 + 1px 白描边，在蓝色实底上保持清晰。
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, ImVec2(a.x + 4.0f, b.y), aurora::col(selEdge), 2.0f);
            dl->AddRect(a, b, aurora::col(selEdge), 2.0f, 0, 1.0f);
        }
        if (sel || !rep.visible) {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
}

// 新建菜单：先选渲染类型，再在下方表单里编辑（名称/配色/作用分子/材质…）。
static void newRepMenu(Workbench& wb, RepresentationController* ctl) {
    const bool hasBackbone = sceneHasProteinBackbone(wb);
    if (ImGui::SmallButton(tr("新建"))) {
        ImGui::OpenPopup("##new_rep_types");
    }
    if (ImGui::BeginPopup("##new_rep_types")) {
        ImGui::TextDisabled("%s", tr("选择渲染类型"));
        ImGui::Separator();
        for (const auto& entry : kTypes) {
            const bool usable = hasBackbone || !isBackboneOnlyStyle(entry.type);
            if (!usable) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(tr(entry.label)) && usable) {
                std::string sel;
                if (wb.selectedMolecule() >= 0 && wb.data() != nullptr) {
                    const std::string nm = groupBaseName(
                        wb.data()->moleculeName(
                            static_cast<size_t>(wb.selectedMolecule())));
                    if (!nm.empty() && nm.find(' ') == std::string::npos) {
                        sel = "resname " + nm;
                    }
                }
                const ColorScheme cs =
                    entry.type == RepresentationType::Wireframe
                        ? ColorScheme::AtomName
                        : ColorScheme::ElementCPK;
                const int32_t nid = ctl->addRepresentation(
                    tr(entry.label), entry.type, sel, cs);
                if (nid >= 0) {
                    wb.setActiveRepIndex(nid);
                }
                ImGui::CloseCurrentPopup();
            }
            if (!usable) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndPopup();
    }
}

// 样式下拉（按结构类型过滤骨架样式；当前值始终保留，避免旧场景无法显示）。
static void styleCombo(Workbench& wb, int32_t id, const Representation& r) {
    const bool hasBackbone = sceneHasProteinBackbone(wb);
    std::string items;
    std::vector<RepresentationType> types;
    for (const auto& entry : kTypes) {
        if (hasBackbone || !isBackboneOnlyStyle(entry.type) || entry.type == r.type) {
            items += tr(entry.label);
            items.push_back('\0');
            types.push_back(entry.type);
        }
    }
    int idx = 0;
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i] == r.type) {
            idx = static_cast<int>(i);
            break;
        }
    }
    aurora::sectionLabel(tr("绘图样式"));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##rep_style", &idx, items.c_str()) && idx >= 0 &&
        idx < static_cast<int>(types.size())) {
        wb.reps()->setType(id, types[static_cast<size_t>(idx)]);
    }
}

[[maybe_unused]] static void renderRepTable(Workbench& wb) {
    RepresentationController* ctl = wb.reps();
    const auto& reps = wb.representations();
    if (ctl == nullptr || reps.empty()) {
        ImGui::TextDisabled("%s", tr("暂无表示，点下方「新建」添加"));
        return;
    }
    const int32_t active = activeId(wb);
    const bool hasBackbone = sceneHasProteinBackbone(wb);
    const float availW = ImGui::GetContentRegionAvail().x;
    // 窄面板用 3 列（样式与配色合并成一格），宽面板才铺开 4 列
    const bool wide = availW >= 430.0f;
    const int cols = wide ? 4 : 3;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoPadOuterX;
    if (!ImGui::BeginTable("##rep_table", cols, flags)) {
        return;
    }
    if (wide) {
        ImGui::TableSetupColumn(tr("名称"), ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn(tr("样式"), ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn(tr("配色"), ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn(tr("选择"), ImGuiTableColumnFlags_WidthStretch, 0.30f);
    } else {
        ImGui::TableSetupColumn(tr("名称"), ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn(tr("样式 / 配色"), ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn(tr("选择"), ImGuiTableColumnFlags_WidthStretch, 0.32f);
    }
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < reps.size(); ++i) {
        const Representation& rep = reps[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 26.0f);
        const int32_t rowWire = static_cast<int32_t>(i);
        const bool sel = (rowWire == active);
        const bool multi = wb.isRepMultiSelected(rowWire);
        // 选中行整行底色（不用 SpanAllColumns 的 Selectable，避免与单元格控件 ID 冲突）
        if (sel || multi) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(sel ? ImGuiCol_Header : ImGuiCol_HeaderHovered,
                                                      sel ? 0.55f : 0.45f));
        }
        ImGui::TableSetColumnIndex(0);
        // 注意：表格里 TableSetColumnIndex 会用「列 ID」覆盖 ID 栈（PushOverrideID），
        // 所以行级 PushID 在单元格内部不生效；每个控件都要用「标签##行号」自带唯一后缀。
        const std::string rowTag = "##r" + std::to_string(i);
        // 隐藏的表示：名字变灰 + 「· 隐藏」，一眼能看出为什么视口里没有它
        std::string shownName = rep.name;
        if (!rep.visible) {
            shownName += std::string(" ") + tr("· 隐藏");
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().textMuted);
        }
        const std::string nameLbl = shownName + rowTag;
        // Ctrl/Shift 点行 = 多选；普通点 = 单选并设为编辑目标
        if (ImGui::Selectable(nameLbl.c_str(), false, ImGuiSelectableFlags_None)) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl || io.KeyShift) {
                wb.toggleRepMultiSelected(rowWire);
            } else {
                wb.clearRepMultiSelection();
                wb.setActiveRepIndex(rowWire);
            }
        }
        if (!rep.visible) {
            ImGui::PopStyleColor();
        }
        const std::string style = engine::representationTypeZh(rep.type);
        const std::string color = engine::colorSchemeZh(rep.colorScheme);
        // v2.9：样式/配色单元格可点击，弹出快速选择（不必先选中行再滚到下方表单）
        const auto popupStyles = [&](int32_t repId) {
            for (const auto& entry : kTypes) {
                const bool cur = (entry.type == rep.type);
                const std::string lbl =
                    std::string(tr(entry.label)) + "##psi" + std::to_string(i);
                const bool usable = hasBackbone || !isBackboneOnlyStyle(entry.type);
                if (!usable) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Selectable(lbl.c_str(), cur) && usable) {
                    ctl->setType(repId, entry.type);
                }
                if (!usable) {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("%s", tr("需要蛋白质骨架（含 CA 原子的链）；"
                                                   "当前结构没有骨架，选了会是空白"));
                    }
                }
            }
        };
        const auto popupColors = [&](int32_t repId) {
            for (int32_t c = 0; c < static_cast<int32_t>(engine::ColorScheme::Count); ++c) {
                const auto cs = static_cast<engine::ColorScheme>(c);
                const bool cur = (cs == rep.colorScheme);
                const std::string lbl =
                    std::string(engine::colorSchemeZh(cs)) + "##pci" + std::to_string(i);
                if (ImGui::Selectable(lbl.c_str(), cur)) {
                    ctl->setColorScheme(repId, cs);
                }
            }
        };
        const int32_t rowId = static_cast<int32_t>(i);
        const std::string popStyle = "##style_pop" + std::to_string(i);
        const std::string popColor = "##color_pop" + std::to_string(i);
        const std::string popBoth = "##style_color_pop" + std::to_string(i);
        // 单元格点击 = 「先把这个表示设为编辑目标，再打开选择器」。
        // 旧行为只打开弹层、不改活动表示，于是"点了样式却改了别的表示" ——
        // 这是用户反馈的"显示视图的选择器 bug"的主因。现在与点名称行一致。
        const auto activateRow = [&]() {
            wb.clearRepMultiSelection();
            wb.setActiveRepIndex(rowId);
        };
        if (wide) {
            ImGui::TableSetColumnIndex(1);
            const std::string styleLbl = style + " ▾##st" + std::to_string(i);
            if (ImGui::Selectable(styleLbl.c_str())) {
                activateRow();
                ImGui::OpenPopup(popStyle.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("点击修改绘制样式（同时切换编辑目标）"));
            }
            if (ImGui::BeginPopup(popStyle.c_str())) {
                popupStyles(rowId);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(2);
            const std::string colorLbl = color + " ▾##cl" + std::to_string(i);
            if (ImGui::Selectable(colorLbl.c_str())) {
                activateRow();
                ImGui::OpenPopup(popColor.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("点击修改配色方案（同时切换编辑目标）"));
            }
            if (ImGui::BeginPopup(popColor.c_str())) {
                popupColors(rowId);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(3);
        } else {
            ImGui::TableSetColumnIndex(1);
            const std::string both = style + " · " + color + " ▾##sc" + std::to_string(i);
            if (ImGui::Selectable(both.c_str())) {
                activateRow();
                ImGui::OpenPopup(popBoth.c_str());
            }
            if (ImGui::BeginPopup(popBoth.c_str())) {
                ImGui::TextDisabled("%s", tr("绘制样式"));
                popupStyles(rowId);
                ImGui::Separator();
                ImGui::TextDisabled("%s", tr("配色"));
                popupColors(rowId);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(2);
        }
        // 选择列：点击弹出「这个表示作用于哪些分子」勾选面板
        const std::string selTxt = rep.selection.empty() ? std::string(tr("全部")) : rep.selection;
        std::string selShort = selTxt;
        if (selShort.size() > 14) {
            selShort = selShort.substr(0, 12) + "…";
        }
        const std::string selLbl = selShort + " ▾##selcell" + std::to_string(i);
        const std::string popSel = "##sel_pop" + std::to_string(i);
        if (ImGui::Selectable(selLbl.c_str())) {
            activateRow();
            ImGui::OpenPopup(popSel.c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", selTxt.c_str());
        }
        if (ImGui::BeginPopup(popSel.c_str())) {
            renderMolScopeEditor(wb, rowId, rep.selection);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        // 行尾「...」菜单：删除等行操作集中到这里，避免误点（原先「×」紧贴选择列，容易误删）
        const std::string menuLbl = "...##rm" + std::to_string(i);
        const std::string popMenu = "##rep_row_menu" + std::to_string(i);
        if (ImGui::SmallButton(menuLbl.c_str())) {
            activateRow();
            ImGui::OpenPopup(popMenu.c_str());
        }
        if (ImGui::BeginPopup(popMenu.c_str())) {
            const std::string delLbl = std::string(tr("删除此表示")) + "##del" + std::to_string(i);
            const std::string tgtLbl = std::string(tr("设为编辑目标")) + "##tgt" + std::to_string(i);
            if (ImGui::MenuItem(delLbl.c_str())) {
                ctl->removeRepresentation(rowId);
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            if (ImGui::MenuItem(tgtLbl.c_str())) {
                wb.setActiveRepIndex(rowId);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    // v2.9：多选批量条（Ctrl/Shift 点行多选后出现）
    const auto& multiSel = wb.repMultiSelection();
    if (multiSel.size() < 2) {
        return;
    }
    aurora::spacer(4.0f);
    ImGui::TextDisabled("%s %d %s", tr("已选"), static_cast<int>(multiSel.size()),
                        tr("个表示"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("批量样式"))) {
        ImGui::OpenPopup("##batch_style");
    }
    if (ImGui::BeginPopup("##batch_style")) {
        for (const auto& entry : kTypes) {
            if (ImGui::Selectable(tr(entry.label))) {
                for (const int32_t id : multiSel) {
                    ctl->setType(id, entry.type);
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("批量配色"))) {
        ImGui::OpenPopup("##batch_color");
    }
    if (ImGui::BeginPopup("##batch_color")) {
        for (int32_t c = 0; c < static_cast<int32_t>(engine::ColorScheme::Count); ++c) {
            const auto cs = static_cast<ColorScheme>(c);
            if (ImGui::Selectable(engine::colorSchemeZh(cs))) {
                for (const int32_t id : multiSel) {
                    ctl->setColorScheme(id, cs);
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("批量可见"))) {
        bool anyHidden = false;
        for (const int32_t id : multiSel) {
            const auto& list = wb.representations();
            if (id >= 0 && static_cast<size_t>(id) < list.size() && !list[id].visible) {
                anyHidden = true;
                break;
            }
        }
        for (const int32_t id : multiSel) {
            ctl->setVisible(id, anyHidden);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("批量删除"))) {
        std::vector<int32_t> victims = multiSel;
        std::sort(victims.rbegin(), victims.rend());
        for (const int32_t id : victims) {
            ctl->removeRepresentation(id);
        }
        wb.clearRepMultiSelection();
    }
}

class VisualizeModule : public IInspectorModule {
public:
    const char* id() const override { return "visualize"; }
    const char* label() const override { return "渲染"; }
    const char* subtitle() const override { return "选择或新建表示，在下方编辑"; }

    void render(Workbench& wb) override {
        RepresentationController* ctl = wb.reps();
        if (ctl == nullptr) {
            ImGui::TextDisabled("%s", tr("表示控制器未就绪"));
            return;
        }
        if (wb.representations().empty()) {
            ImGui::TextWrapped("%s", tr("当前没有任何表示。创建一个球棍表示即可开始。"));
            aurora::spacer(6.0f);
            if (aurora::chip(tr("创建球棍表示"), true)) {
                const int32_t id = ctl->addRepresentation(
                    tr("球棍"), RepresentationType::BallAndStick, std::string(),
                    ColorScheme::ElementCPK);
                if (id >= 0) {
                    wb.setActiveRepIndex(id);
                }
            }
            return;
        }

        const int32_t id = activeId(wb);
        // v2.9 修复：这里必须**按值**取快照。之前是 const Representation&，
        // 一旦本帧内「新建/删除」导致 vector 重新分配，引用立刻悬垂 → 0xC0000005 崩溃
        // （复现：有 1 个表示时点「新建」，日志 4×event type=3 后 CRASH）。
        const Representation r = wb.representations()[static_cast<size_t>(id)];
        bool removed = false;

        renderRenderQuality(wb);


        auto drawReps = [&]() {
            aurora::sectionLabel(tr("表示"), tr("点击选中 · 在下方编辑"));
            renderRepList(wb);
            newRepMenu(wb, ctl);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("复制"))) {
                // v2.12：复制当前表示（样式 + 选择 + 配色 + 材质 + 几何一并带上）
                const int32_t nid = ctl->addRepresentation(r.name + "*", r.type, r.selection,
                                                           r.colorScheme);
                if (nid >= 0) {
                    ctl->setRadiusScale(nid, r.radiusScale);
                    ctl->setBondScale(nid, r.bondScale);
                    ctl->setMaterialPreset(nid, r.materialPreset, r.ambient, r.diffuse,
                                           r.specular, r.shininess, r.metallic, r.roughness,
                                           r.opacity);
                    wb.setActiveRepIndex(nid);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("删除"))) {
                ctl->removeRepresentation(id);
                removed = true;
                return;
            }
            ImGui::SameLine();
            bool visible = r.visible;
            if (ImGui::Checkbox(tr("可见"), &visible)) {
                ctl->setVisible(id, visible);
            }
            aurora::spacer(2.0f);
            const std::string curSelTxt = r.selection.empty() ? std::string(tr("全部")) : r.selection;
            const std::string scopeHdr =
                std::string(tr("作用分子")) + "  [" + curSelTxt + "]##molscope";
            if (ImGui::TreeNodeEx(scopeHdr.c_str(),
                                  ImGuiTreeNodeFlags_SpanAvailWidth |
                                      ImGuiTreeNodeFlags_FramePadding)) {
                renderMolScopeEditor(wb, id, r.selection);
                ImGui::TreePop();
            }
        };

        // v2.9：面板够宽时自动变两列（左：分子；右：表示），窄时仍是单列
        drawReps();
        if (removed) {
            return;
        }

        aurora::spacer(4.0f);
        // v2.12：样式/配色已可在上表单元格里直接点选，这里不再重复放两个下拉。
        // 只保留一行「当前表示」摘要（下面是该表示的材质/颜色/几何参数归属说明），
        // 以及表格里没有的「材质」维度。
        {
            aurora::sectionLabel(tr("当前表示"), tr("样式 / 配色 / 作用分子都在下方编辑"));
            const std::string curStyle = engine::representationTypeZh(r.type);
            const std::string curColor = engine::colorSchemeZh(r.colorScheme);
            char line[256];
            std::snprintf(line, sizeof(line), "%s   %s · %s", r.name.c_str(),
                          curStyle.c_str(), curColor.c_str());
            ImGui::TextDisabled("%s", line);
            if (isBackboneOnlyStyle(r.type) && !sceneHasProteinBackbone(wb)) {
                ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().warn);
                ImGui::TextWrapped("%s", tr("该样式只适用于蛋白质骨架；当前结构没有 CA 骨架，"
                                            "视口会是空白。"));
                ImGui::PopStyleColor();
                if (aurora::chip(tr("改回球棍"), false,
                                 tr("切回通用样式，立即恢复可见"))) {
                    ctl->setType(id, RepresentationType::BallAndStick);
                }
            }
        }
        aurora::spacer(2.0f);
        styleCombo(wb, id, r);
        colorCombo(wb, id, r);
        aurora::spacer(2.0f);
        materialCombo(wb, id, r);

        // 分子显示视图叠加（v2.12）：坐标轴方向 / 比例尺与尺寸，可分别关闭。
        aurora::spacer(2.0f);
        aurora::sectionLabel(tr("视图叠加"), tr("可分别关闭"));
        {
            bool axes = wb.showViewAxes();
            if (aurora::toggleRow(tr("坐标轴方向 (X/Y/Z)"), &axes,
                                  tr("左下角显示世界坐标轴朝向"))) {
                wb.setShowViewAxes(axes);
                if (wb.viewport() != nullptr) {
                    wb.viewport()->setViewOverlay(wb.showViewAxes(), wb.showViewScale());
                }
            }
            bool scale = wb.showViewScale();
            if (aurora::toggleRow(tr("比例尺与尺寸 (Å)"), &scale,
                                  tr("右下角比例尺 + 右上角场景包围盒尺寸"))) {
                wb.setShowViewScale(scale);
                if (wb.viewport() != nullptr) {
                    wb.viewport()->setViewOverlay(wb.showViewAxes(), wb.showViewScale());
                }
            }
        }

        // 「调色盘 · 原子颜色」：统一色组件（与设置页的"背景/主题色"分开）。
        // 引擎约定：Uniform 配色的颜色存在材质 diffuse，顶点色为白（v2.3 修正）。
        aurora::sectionLabel(tr("调色盘 · 原子颜色"), tr("统一色"));
        if (r.colorScheme != ColorScheme::Uniform) {
            if (aurora::chip(tr("改为统一色"), false,
                             tr("把当前表示从元素/链/残基配色切到统一色"))) {
                ctl->setColorScheme(id, ColorScheme::Uniform);
            }
        } else {
            float ucol[3] = {r.diffuse[0], r.diffuse[1], r.diffuse[2]};
            if (ImGui::ColorEdit3(
                    "##uniform", ucol,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                applyUniformColor(ctl, id, r, ucol);
            }
            // 12 色预设：低饱和（学术插图感），点击即应用（先切统一色，再写颜色）
            static const float kSwatches[12][3] = {
                {0.95f, 0.95f, 0.96f}, {0.80f, 0.80f, 0.82f},
                {0.60f, 0.62f, 0.66f}, {0.22f, 0.24f, 0.28f},
                {0.35f, 0.50f, 0.72f}, {0.22f, 0.58f, 0.62f},
                {0.35f, 0.60f, 0.45f}, {0.78f, 0.68f, 0.42f},
                {0.78f, 0.48f, 0.30f}, {0.70f, 0.32f, 0.30f},
                {0.55f, 0.42f, 0.68f}, {0.80f, 0.55f, 0.62f},
            };
            for (int i = 0; i < 12; ++i) {
                ImGui::PushID(i);
                const ImVec4 sw(kSwatches[i][0], kSwatches[i][1], kSwatches[i][2], 1.0f);
                if (ImGui::ColorButton(
                        "##sw", sw,
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                        ImVec2(22, 22))) {
                    ctl->setColorScheme(id, ColorScheme::Uniform);
                    applyUniformColor(ctl, id, r, kSwatches[i]);
                }
                ImGui::PopID();
                if (i < 11) {
                    ImGui::SameLine(0, 4);
                }
            }
        }

        aurora::spacer(2.0f);
        aurora::sectionLabel(tr("几何"));
        float radius = r.radiusScale;
        if (aurora::sliderNum(tr("原子半径"), &radius, 0.05f, 5.0f, "%.2f")) {
            ctl->setRadiusScale(id, radius);
        }
        float bond = r.bondScale;
        if (aurora::sliderNum(tr("键半径"), &bond, 0.05f, 5.0f, "%.2f")) {
            ctl->setBondScale(id, bond);
        }
        float opacity = r.opacity;
        if (aurora::sliderNum(tr("不透明度"), &opacity, 0.05f, 1.0f, "%.2f",
                             0.01f,
                             tr("滑块右侧窄框可直接输入精确值（框内按住可拖动微调）"))) {
            setOpacity(ctl, id, r, opacity);
        }

        // 材质微调：颜色用取色器，其它沿用数值滑杆；改任一项即「自定义材质」
        if (ImGui::TreeNodeEx(tr("材质微调"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            float ambient[4] = {r.ambient[0], r.ambient[1], r.ambient[2], 1.0f};
            float diffuse[4] = {r.diffuse[0], r.diffuse[1], r.diffuse[2], 1.0f};
            float specular[4] = {r.specular[0], r.specular[1], r.specular[2], 1.0f};
            bool matChanged = false;
            matChanged |= ImGui::ColorEdit3(tr("环境色"), ambient,
                                            ImGuiColorEditFlags_NoInputs);
            matChanged |= ImGui::ColorEdit3(tr("漫反射色"), diffuse,
                                            ImGuiColorEditFlags_NoInputs);
            matChanged |= ImGui::ColorEdit3(tr("高光色"), specular,
                                            ImGuiColorEditFlags_NoInputs);
            float shininess = r.shininess;
            float metallic = r.metallic;
            float roughness = r.roughness;
            matChanged |=
            aurora::sliderNum(tr("光泽度"), &shininess, 1.0f, 256.0f, "%.0f", 1.0f, tr("Blinn-Phong 高光指数：越大高光越锐、越像玻璃/金属"));
            matChanged |= aurora::sliderNum(tr("金属度"), &metallic, 0.0f, 1.0f, "%.2f", 0.01f, tr("0 = 绝缘体，1 = 金属（影响高光颜色与强度）"));
            matChanged |= aurora::sliderNum(tr("粗糙度"), &roughness, 0.0f, 1.0f, "%.2f", 0.01f, tr("0 = 镜面般光滑，1 = 完全漫反射"));
            if (matChanged) {
                ctl->setMaterial(id, r.materialPreset, ambient, diffuse, specular,
                                 shininess, metallic, roughness, r.opacity);
            }
            if (aurora::chip(tr("恢复预设材质"), false)) {
                applyMaterial(ctl, id, r.materialPreset);
            }
            ImGui::TreePop();
        }

        // 表面类样式的参数
        if (r.type == RepresentationType::Surface ||
            r.type == RepresentationType::QuickSurf) {
            aurora::sectionLabel(tr("表面参数"));
            float iso = r.surfIsovalue;
            if (aurora::sliderNum(tr("等值面"), &iso, 0.05f, 2.0f, "%.2f", 0.01f, tr("表面等值面（vdW 半径倍数）：越小表面越贴合原子"))) {
                ctl->setSurfIsovalue(id, iso);
            }
            float grid = r.surfGridSpacing;
            if (aurora::sliderNum(tr("网格间距 (Å)"), &grid, 0.1f, 2.0f, "%.2f", 0.05f, tr("网格间距：越小越精细但更慢，0.5 为常用值"))) {
                ctl->setSurfGridSpacing(id, grid);
            }
            bool sys = r.systemSurface;
            if (aurora::toggleRow(tr("整体系合并表面"), &sys,
                                  tr("整个体系生成一张表面；溶剂化体系显著更快"))) {
                ctl->setSystemSurface(id, sys);
            }
            if (r.systemSurface) {
                ImGui::SameLine();
                aurora::badge(tr("整体系"), aurora::BadgeKind::Info);
            }
        }

        if (r.type == RepresentationType::EspSurface) {
            aurora::sectionLabel(tr("ESP"));
            float iso = r.espIsovalue;
            if (aurora::sliderNum(tr("等值面 (a.u.)"), &iso, 0.0005f, 0.02f, "%.4f", 0.0005f, tr("电子密度等值面阈值：决定 ESP 表面的位置"))) {
                ctl->setEspIsovalue(id, iso);
            }
            if (aurora::chip(tr("打开 ESP 面板配置数据与色标"), false)) {
                wb.setModule("esp");
            }
        }

        // 选择表达式
        aurora::spacer(2.0f);
        aurora::sectionLabel(tr("选择表达式"), tr("留空 = 全部原子"));
        if (selRep_ != id || selSource_ != r.selection) {
            std::snprintf(selBuf_, sizeof(selBuf_), "%s", r.selection.c_str());
            selRep_ = id;
            selSource_ = r.selection;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool entered =
            ImGui::InputText("##selection", selBuf_, sizeof(selBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
            entity::Selection probe;
            const bool empty = selBuf_[0] == '\0';
            const bool ok =
                empty || probe.parse(selBuf_, entity::ParseTable::defaultTable());
            if (ok) {
                ctl->setSelection(id, selBuf_);
                selSource_ = selBuf_;
                selValid_ = true;
                // 只读预校验：立即统计命中原子数（evaluate 不改场景、不置脏位），
                // 用户才能立刻知道表达式是否真的圈到了原子 —— 否则只有"没变化"的
                // 错觉（比如类型名拼错时选择框里是全选态，但画面毫无反应）。
                selStatus_.clear();
                if (!empty && wb.scene() != nullptr) {
                    entity::FrameData frame;
                    frame.positions = &wb.scene()->coordsSoA().positions;
                    probe.evaluate(*wb.scene(), frame, 0);
                    size_t hits = 0;
                    for (bool in : probe.mask()) {
                        if (in) {
                            ++hits;
                        }
                    }
                    selStatus_ = std::string(tr("已应用 · 命中 ")) +
                                 std::to_string(hits) + tr(" 原子");
                }
            } else {
                selValid_ = false;
            }
        }
        if (!selValid_) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().danger);
            ImGui::TextWrapped("%s", tr("表达式无法解析，未应用（保留上一个有效表达式）"));
            ImGui::PopStyleColor();
            selValid_ = true;
        } else if (!selStatus_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().ok);
            ImGui::TextWrapped("%s", selStatus_.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled("%s", tr("例：not element H · resid 1-20 · within 4 of element Fe"));

        // 作用分子（v2.19）：完整编辑器（快捷按钮 / 搜索 / 类型计数 / 类型范围）
        // 与表示表格的「选择」弹层共用同一份实现，避免两处行为漂移。
        if (wb.data() != nullptr &&
            ImGui::TreeNodeEx(tr("作用分子（多选）"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            ImGui::Indent();
            renderMolScopeEditor(wb, id, r.selection);
            ImGui::Unindent();
            ImGui::TreePop();
        }

        // —— 表面（v2.7）：原左侧「表面」卡片并入这里，避免两处重复维护 ——
        aurora::spacer(6.0f);
        if (ImGui::CollapsingHeader(tr("表面"))) {
            ImGui::Indent();
            renderSurfaceSection(wb);
            ImGui::Unindent();
        }
    }

private:
    char selBuf_[512] = {};
    int32_t selRep_ = -2;
    std::string selSource_;
    bool selValid_ = true;
    std::string selStatus_;   // 合法表达式的命中统计（"已应用 · 命中 N 原子"）
};

// =============================================================================
// 2. 分子（Scene）—— 实例与体系
// =============================================================================
class SceneModule : public IInspectorModule {
public:
    const char* id() const override { return "scene"; }
    const char* label() const override { return "分子"; }
    const char* subtitle() const override { return "实例、体系与可见性"; }

    void render(Workbench& wb) override {
        DataManager* dm = wb.data();
        SceneController* ctl = wb.sceneCtl();
        if (dm == nullptr || ctl == nullptr) {
            ImGui::TextDisabled("%s", tr("数据未就绪"));
            return;
        }

        if (aurora::chip(tr("打开结构文件"), true)) {
            wb.openFileDialog();
        }
        ImGui::SameLine();
        if (aurora::chip(tr("导入轨迹"), false)) {
            wb.importTrajectoryDialog();
        }

        aurora::spacer(6.0f);
        renderMoleculeSection(wb);

        aurora::sectionLabel(tr("显示选项"));
        bool hideH = wb.hydrogenHidden();
        if (aurora::toggleRow(tr("隐藏氢原子"), &hideH,
                              tr("等价于把全部表示的选择改成 not element H"))) {
            wb.setHydrogenHidden(hideH);
        }
        if (aurora::chip(tr("取景全部"), false)) {
            wb.frameCamera();
        }
        ImGui::SameLine();
        if (aurora::chip(tr("重置视角"), false)) {
            wb.resetCamera();
        }
    }
};

// =============================================================================
// 3. 表面（Surface）—— v2.7 起并入「显示」模块，不再单独出现在左侧导航
// =============================================================================
void renderSurfaceSection(Workbench& wb) {
    {
        RepresentationController* ctl = wb.reps();
        if (ctl == nullptr) {
            ImGui::TextDisabled("%s", tr("表示控制器未就绪"));
            return;
        }
        const auto& reps = wb.representations();
        const auto isSurfaceRep = [](RepresentationType t) {
            return t == RepresentationType::Surface || t == RepresentationType::QuickSurf;
        };
        // 编辑**当前选中**的表面表示，而不是永远第一条：同时开着「分子表面」与
        // 「快速表面」时，面板改的必须是用户正在看的那条，否则改动像没生效。
        std::vector<int32_t> surfaceIds;
        for (int32_t i = 0; i < static_cast<int32_t>(reps.size()); ++i) {
            if (isSurfaceRep(reps[static_cast<size_t>(i)].type)) {
                surfaceIds.push_back(i);
            }
        }
        int32_t id = -1;
        const int32_t act = activeId(wb);
        if (act >= 0 && isSurfaceRep(reps[static_cast<size_t>(act)].type)) {
            id = act;
        }
        if (surfaceIds.empty()) {
            ImGui::TextWrapped("%s", tr("尚未创建表面表示。"));
            aurora::spacer(6.0f);
            if (aurora::chip(tr("创建快速表面"), true)) {
                const int32_t nid = ctl->addRepresentation(
                    tr("表面"), RepresentationType::QuickSurf, std::string(),
                    ColorScheme::ElementCPK);
                if (nid >= 0) {
                    wb.setActiveRepIndex(nid);
                }
            }
            ImGui::SameLine();
            if (aurora::chip(tr("创建分子表面"), false)) {
                const int32_t nid = ctl->addRepresentation(
                    tr("表面"), RepresentationType::Surface, std::string(),
                    ColorScheme::ElementCPK);
                if (nid >= 0) {
                    wb.setActiveRepIndex(nid);
                }
            }
            ImGui::TextDisabled("%s",
                                tr("表面样式会覆盖分子外观；通常配合半透明球棍一起使用。"));
            return;
        }
        // 当前表示不是表面（或还没选）→ 列出现有表面供点选；点选即切换 activeRepIndex。
        if (id < 0) {
            id = surfaceIds.front();
        }
        if (surfaceIds.size() > 1) {
            const std::string preview = repLabel(reps, static_cast<size_t>(id));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##surface_pick", preview.c_str())) {
                for (int32_t sid : surfaceIds) {
                    ImGui::PushID(sid);
                    const std::string label = repLabel(reps, static_cast<size_t>(sid));
                    if (ImGui::Selectable(label.c_str(), sid == id)) {
                        wb.setActiveRepIndex(sid);
                        id = sid;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        const Representation& r = reps[static_cast<size_t>(id)];
        ImGui::TextColored(aurora::palette().textMuted, "%s",
                           tr("正在编辑：") );
        ImGui::SameLine();
        ImGui::TextUnformatted(repLabel(reps, static_cast<size_t>(id)).c_str());

        aurora::sectionLabel(tr("绘制模式"));
        int drawMode = std::clamp<int>(r.surfaceDrawMode, 0, 2);
        std::string drawModeItems;
        drawModeItems += tr("实心");
        drawModeItems.push_back('\0');
        drawModeItems += tr("线框");
        drawModeItems.push_back('\0');
        drawModeItems += tr("半透明");
        drawModeItems.push_back('\0');
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##surface_draw_mode", &drawMode, drawModeItems.c_str())) {
            ctl->setSurfaceDrawMode(id, static_cast<uint8_t>(drawMode));
        }
        aurora::helpTip(tr("实心 = 填充三角面；线框 = 只画网格边；半透明 = 混合管线，便于看到内部球棍。"));

        aurora::sectionLabel(tr("网格质量"));
        static const struct { const char* name; float spacing; } kQuality[] = {
            {"低", 1.00f}, {"中", 0.50f}, {"高", 0.25f}, {"超高", 0.15f}};
        int q = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(kQuality[i].spacing - r.surfGridSpacing) < 0.005f) {
                q = i;
            }
        }
        ImGui::SetNextItemWidth(-1.0f);
        int qSel = q < 0 ? 4 : q;
        const char* qItems = "低\0中\0高\0超高\0自定义\0";
        if (ImGui::Combo("##quality", &qSel, qItems)) {
            if (qSel < 4) {
                ctl->setSurfGridSpacing(id, kQuality[qSel].spacing);
            }
        }
        aurora::helpTip(tr("网格间距越小越精细、越慢；「自定义」表示当前值不在档位内。"));

        float iso = r.surfIsovalue;
        if (aurora::sliderNum(tr("等值面阈值"), &iso, 0.05f, 2.0f, "%.2f", 0.01f)) {
            ctl->setSurfIsovalue(id, iso);
        }
        // 平滑：vdW 并集在溶剂体系里会生成"每个水分子一个疙瘩"的壳；1–3 次
        // 3×3×3 盒式平滑后是接近 QuickSurf/SES 的连续表面。
        int smooth = std::clamp(r.surfSmooth, 0, 3);
        if (aurora::stepper(tr("表面平滑"), &smooth, 1, 0, 3,
                            tr("对 vdW 标量场做 3×3×3 平滑的次数；0 = 关闭（保留原始 vdW 疙瘩）"))) {
            ctl->setSurfSmooth(id, smooth);
        }
        aurora::helpTip(tr("溶剂盒整体系表面建议 1–2 次平滑；溶质表面 1 次即可，"
                           "过高会让小分子表面涨大、细节消失。"));
        if (aurora::chip(tr("仅溶质（去溶剂）"), false,
                         tr("把作用分子改为排除 SOL/WAT 等水分子，只看溶质表面"))) {
            ctl->setSelection(id,
                              "not resname SOL and not resname WAT and not resname HOH "
                              "and not resname TIP3 and not resname TIP3P and not resname H2O");
        }
        bool sys = r.systemSurface;
        if (aurora::toggleRow(tr("整体系合并表面"), &sys,
                              tr("溶剂化体系强烈建议开启"))) {
            ctl->setSystemSurface(id, sys);
        }
        float opacity = r.opacity;
        if (aurora::sliderNum(tr("不透明度"), &opacity, 0.05f, 1.0f, "%.2f", 0.01f)) {
            setOpacity(ctl, id, r, opacity);
        }
        bool vis = r.visible;
        if (aurora::toggleRow(tr("可见"), &vis)) {
            ctl->setVisible(id, vis);
        }
    }
}

// =============================================================================
// 4. ESP（静电势映射表面）
// =============================================================================
class EspModule : public IInspectorModule {
public:
    const char* id() const override { return "esp"; }
    const char* label() const override { return "ESP"; }
    const char* subtitle() const override { return "电子密度等值面 + 静电势着色"; }

    void render(Workbench& wb) override {
        RepresentationController* ctl = wb.reps();
        if (ctl == nullptr) {
            ImGui::TextDisabled("%s", tr("表示控制器未就绪"));
            return;
        }
        const auto& reps = wb.representations();
        int32_t id = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(reps.size()); ++i) {
            if (reps[static_cast<size_t>(i)].type == RepresentationType::EspSurface) {
                id = i;
                break;
            }
        }
        if (id < 0) {
            ImGui::TextWrapped("%s",
                               tr("ESP 表面需要两个 Gaussian cube 文件：电子密度（几何）"
                                  "与静电势（颜色）。"));
            aurora::spacer(6.0f);
            if (aurora::chip(tr("创建 ESP 表示"), true)) {
                const int32_t nid = ctl->addRepresentation(
                    tr("ESP"), RepresentationType::EspSurface, std::string(),
                    ColorScheme::ElementCPK);
                if (nid >= 0) {
                    wb.setActiveRepIndex(nid);
                }
            }
            return;
        }
        const Representation& r = reps[static_cast<size_t>(id)];

        aurora::sectionLabel(tr("数据文件"));
        const std::string cubeFilter =
            "Gaussian cube 文件 (*.cube;*.cub)\0*.cube;*.cub\0所有文件 (*.*)\0*.*\0";
        auto pathRow = [&](const char* label, std::string& path, const char* hint) {
            // 两行（密度/静电势）各有一个 "##path" InputText 与「浏览…」按钮；
            // 不隔离 ID 会触发 ImGui「2 visible items with conflicting ID」断言
            // （同样的问题在 reps_tool 里已用 PushID 规避）。
            ImGui::PushID(label);
            ImGui::TextColored(aurora::palette().textMuted, "%s", tr(label));
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", path.empty() ? tr(hint) : path.c_str());
            ImGui::SetNextItemWidth(-84.0f);
            ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("浏览…"))) {
                const std::string chosen = wb.chooseFile(cubeFilter.c_str(), "cube");
                if (!chosen.empty()) {
                    path = chosen;
                    wb.notify(tr("已选择 cube 文件"));
                }
            }
            ImGui::PopID();
        };
        std::string density = r.espDensityCubePath;
        std::string potential = r.espPotentialCubePath;
        pathRow("密度 cube（几何）", density, "（未选择）");
        pathRow("静电势 cube（颜色）", potential, "（必填）");
        if (density != r.espDensityCubePath) {
            ctl->setEspDensityCubePath(id, density);
        }
        if (potential != r.espPotentialCubePath) {
            ctl->setEspPotentialCubePath(id, potential);
        }
        if (r.espDensityCubePath.empty() || r.espPotentialCubePath.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().warn);
            ImGui::TextWrapped("%s", tr("补齐两个 cube 文件后表面才会出现。"));
            ImGui::PopStyleColor();
        }

        aurora::sectionLabel(tr("等值面"));
        float iso = r.espIsovalue;
        if (aurora::sliderRow(tr("密度阈值 (a.u.)"), &iso, 0.0005f, 0.02f, "%.4f")) {
            ctl->setEspIsovalue(id, iso);
        }
        ImGui::TextDisabled("%s", tr("常用：0.001（约 vdW 面）· 0.0005（更外扩）· 0.005（更贴身）"));

        aurora::sectionLabel(tr("色标"));
        bool autoRange = r.espAutoRange;
        if (aurora::toggleRow(tr("自动范围"), &autoRange,
                              tr("按静电势数据自动定上下限"))) {
            ctl->setEspAutoRange(id, autoRange);
        }
        if (!r.espAutoRange) {
            float vmin = r.espVminKcal;
            float vmax = r.espVmaxKcal;
            bool changed = false;
            if (aurora::sliderRow(tr("下限 (kcal/mol)"), &vmin, -300.0f, 300.0f, "%.1f")) {
                changed = true;
            }
            if (aurora::sliderRow(tr("上限 (kcal/mol)"), &vmax, -300.0f, 300.0f, "%.1f")) {
                changed = true;
            }
            if (changed) {
                ctl->setEspRange(id, vmin, std::max(vmax, vmin + 1.0f));
            }
        }
        int cmap = std::clamp<int>(r.espColormap, 0,
                                   static_cast<int>(engine::EspColormap::Count) - 1);
        std::string cmapItems;
        for (int i = 0; i < static_cast<int>(engine::EspColormap::Count); ++i) {
            cmapItems += tr(engine::espColormapLabelZh(static_cast<engine::EspColormap>(i)));
            cmapItems.push_back('\0');
        }
        aurora::sectionLabel(tr("配色方案"));
        if (ImGui::Combo("##cmap", &cmap, cmapItems.c_str())) {
            ctl->setEspColormap(id, cmap);
        }
        bool invert = r.espInvert;
        if (aurora::toggleRow(tr("翻转色标"), &invert)) {
            ctl->setEspInvert(id, invert);
        }
        float opacity = r.opacity;
        if (aurora::sliderNum(tr("不透明度"), &opacity, 0.05f, 1.0f, "%.2f", 0.01f)) {
            setOpacity(ctl, id, r, opacity);
        }
        aurora::helpTip(tr("默认半透明，便于同时看见内部球棍；色标条与极值标注在视口右下角。"));
    }
};

// =============================================================================
// 5. 轨道 / 体数据（Orbital）
// =============================================================================
class OrbitalModule : public IInspectorModule {
public:
    const char* id() const override { return "orbital"; }
    const char* label() const override { return "轨道"; }
    const char* subtitle() const override { return "cube 体数据的等值面可视化"; }

    void render(Workbench& wb) override {
        IoController* io = wb.io();
        const std::string src = io != nullptr ? io->currentSourcePath() : std::string();
        const bool isCube = src.size() > 5 &&
                            (src.compare(src.size() - 5, 5, ".cube") == 0 ||
                             src.compare(src.size() - 4, 4, ".cub") == 0);
        if (isCube && src != cubePath_) {
            entity::CubeVolume vol;
            std::string err;
            loaded_ = entity::parseCubeFile(src, vol, &err);
            if (loaded_) {
                cube_ = std::move(vol);
            } else {
                error_ = err;
            }
            cubePath_ = src;
        }

        if (!isCube && !loaded_) {
            ImGui::TextWrapped("%s",
                               tr("打开一个 Gaussian cube 文件后，这里会显示体数据统计并"
                                  "可一键生成等值面。"));
            aurora::spacer(6.0f);
            if (aurora::chip(tr("打开 cube 文件"), true)) {
                wb.openFileDialog();
            }
            return;
        }
        if (!loaded_) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().danger);
            ImGui::TextWrapped("%s", tr("cube 解析失败："));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", error_.c_str());
            return;
        }

        aurora::sectionLabel(tr("体数据"));
        aurora::kvRow(tr("来源"), cubePath_.c_str());
        char dims[64];
        std::snprintf(dims, sizeof(dims), "%d × %d × %d", cube_.dims[0], cube_.dims[1],
                      cube_.dims[2]);
        aurora::kvRow(tr("网格"), dims);
        char range[96];
        std::snprintf(range, sizeof(range), "%.4f … %.4f", cube_.minValue(),
                      cube_.maxValue());
        aurora::kvRow(tr("数值范围"), range);
        char atoms[32];
        std::snprintf(atoms, sizeof(atoms), "%zu", cube_.atoms.size());
        aurora::kvRow(tr("原子数"), atoms);

        aurora::sectionLabel(tr("等值面"));
        aurora::sliderRow(tr("等值面阈值"), &iso_, 0.0005f, 0.5f, "%.4f");
        ImGui::TextDisabled("%s", tr("阈值取在数值范围内才会出现闭合等值面。"));

        aurora::spacer(6.0f);
        if (aurora::chip(tr("生成等值面表示"), true)) {
            RepresentationController* ctl = wb.reps();
            if (ctl != nullptr) {
                const int32_t id = ctl->addRepresentation(
                    tr("体数据等值面"), RepresentationType::EspSurface, std::string(),
                    ColorScheme::ElementCPK);
                if (id >= 0) {
                    ctl->setEspDensityCubePath(id, cubePath_);
                    ctl->setEspPotentialCubePath(id, cubePath_);
                    ctl->setEspIsovalue(id, iso_);
                    ctl->setEspAutoRange(id, true);
                    wb.setActiveRepIndex(id);
                    wb.notify(tr("已生成等值面（颜色即体数据数值）"));
                }
            }
        }
        aurora::helpTip(tr("同一 cube 同时作为几何与着色数据：颜色表示该点的数值大小，"
                           "因此可以当作轨道/密度等值面使用。"));
    }

private:
    std::string cubePath_;
    std::string error_;
    entity::CubeVolume cube_;
    bool loaded_ = false;
    float iso_ = 0.02f;
};

// =============================================================================
// 6. 轨迹（Trajectory）
// =============================================================================
class TrajectoryModule : public IInspectorModule {
public:
    const char* id() const override { return "trajectory"; }
    const char* label() const override { return "轨迹"; }
    const char* subtitle() const override { return "轨迹文件、帧播放与时间轴"; }

    void render(Workbench& wb) override {
        TimelineController* tl = wb.timeline();
        DataManager* dm = wb.data();
        if (tl == nullptr || dm == nullptr) {
            ImGui::TextDisabled("%s", tr("时间轴未就绪"));
            return;
        }

        // —— 轨迹文件：每个体系一条（体系级轨迹挂在成员实例上，取首个成员即可）——
        aurora::sectionLabel(tr("轨迹文件"), tr("每个体系一条"));
        struct TrajRow {
            uint32_t systemId = 0;
            std::string system;
            std::string source;
            size_t frames = 0;
            float dt = 0.0f;
            uint32_t firstInstance = 0;
        };
        std::vector<TrajRow> rows;
        for (size_t s = 0; s < dm->systemCount(); ++s) {
            if (dm->systemMemberCount(s) == 0) {
                continue;
            }
            const uint32_t first = dm->systemMemberInstance(s, 0);
            const size_t fr = dm->frameCount(first);
            if (fr <= 1) {
                continue;
            }
            TrajRow r;
            r.systemId = dm->systemId(s);
            r.system = dm->systemName(s);
            r.source = dm->moleculeTrajectorySource(first);
            r.frames = fr;
            r.dt = dm->moleculeTrajectoryDt(first);
            r.firstInstance = first;
            rows.push_back(std::move(r));
        }
        if (rows.empty()) {
            ImGui::TextWrapped("%s", tr("当前没有轨迹。导入 DCD/XTC/TRR 后，这里会列出文件、"
                                        "所属体系、帧数与 dt。"));
        } else {
            for (size_t i = 0; i < rows.size(); ++i) {
                const TrajRow& r = rows[i];
                ImGui::PushID(static_cast<int>(i));
                const std::string title =
                    r.source.empty() ? std::string(tr("（未命名轨迹）")) : r.source;
                if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                                                         ImGuiTreeNodeFlags_SpanAvailWidth)) {
                    aurora::kvRow(tr("体系"), r.system.c_str());
                    aurora::kvRow(tr("帧数"), std::to_string(r.frames).c_str());
                    char dtBuf[96];
                    std::snprintf(dtBuf, sizeof(dtBuf), "%.2f ps × %zu = %.1f ps", r.dt,
                                  r.frames - 1, r.dt * static_cast<float>(r.frames - 1));
                    aurora::kvRow(tr("时长"), dtBuf);
                    if (aurora::chip(tr("播放此体系"), false,
                                     tr("把时间轴切到这个体系并回到首帧"))) {
                        tl->setActive(r.firstInstance);
                        tl->seek(0);
                    }
                    ImGui::SameLine();
                    if (aurora::chip(tr("卸载"), false, tr("移除该体系的轨迹，退回静态单帧"))) {
                        if (wb.sceneCtl() != nullptr) {
                            wb.sceneCtl()->clearSystemTrajectory(r.systemId);
                        }
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        aurora::spacer(4.0f);
        if (aurora::chip(tr("导入轨迹"), true, tr("DCD / XTC / TRR"))) {
            wb.importTrajectoryDialog();
        }

        const uint64_t frames = tl->frameCount();
        if (frames <= 1) {
            aurora::spacer(6.0f);
            ImGui::TextWrapped("%s", tr("当前没有可播放的帧数据（单帧场景）。"));
            return;
        }
        const int last = static_cast<int>(frames - 1);

        // —— 播放 ——
        aurora::spacer(6.0f);
        aurora::sectionLabel(tr("播放"));
        if (aurora::chip(tl->playing() ? tr("暂停") : tr("播放"), tl->playing())) {
            if (tl->playing()) {
                tl->pause();
            } else {
                tl->play();
            }
        }
        ImGui::SameLine();
        if (aurora::chip(tr("停止"), false)) {
            tl->stop();
        }
        ImGui::SameLine();
        if (aurora::chip(tr("上一帧"), false)) {
            tl->step(-1);
        }
        ImGui::SameLine();
        if (aurora::chip(tr("下一帧"), false)) {
            tl->step(1);
        }

        aurora::spacer(4.0f);
        int frame = static_cast<int>(tl->frame());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##frame", &frame, 0, last)) {
            tl->seek(static_cast<uint64_t>(std::clamp(frame, 0, last)));
        }
        char info[96];
        std::snprintf(info, sizeof(info), "%s %d / %d   (0 - %d)", tr("帧"), frame + 1, last + 1,
                      last);
        ImGui::TextDisabled("%s", info);

        // 启动时沿用 settings.json 的播放倍速（lastSpeed_ 初值 -1：首帧只同步滑块
        // 显示，不调用 setter）；否则滑块默认 1.0 会在第一帧把用户设置覆盖掉。
        // setter 只在控件真的返回值变化时调用（不每帧 toast/touch 调度器）。
        if (lastSpeed_ < 0.0f) {
            speed_ = std::clamp(tl->speed(), 0.1f, 10.0f);
            lastSpeed_ = speed_;
        }
        if (aurora::sliderNum(tr("播放速度"), &speed_, 0.1f, 10.0f, "%.2f×", 0.1f)) {
            tl->setSpeed(speed_);
            lastSpeed_ = speed_;
        }
        // 播放节奏（v2.19）：默认「动态 FPS」= 播放间隔跟随真实渲染帧率（渲染 dt 的
        // EMA），再用目标帧率上限与追帧上限夹住；高刷屏不会把轨迹"加速播放"，掉帧
        // 也不会一次性追几十帧。固定 12/20/24/30/60/120 FPS 可降低 QuickSurf 预计算压力。
        static const char* const kPlaybackModes[] = {
            u8"动态 FPS", u8"刷新率同步", "12 FPS", "20 FPS",
            "24 FPS",     "30 FPS",     "60 FPS", "120 FPS"};
        static const float kPlaybackFps[] = {0.0f, 0.0f, 12.0f, 20.0f,
                                            24.0f, 30.0f, 60.0f, 120.0f};
        constexpr int kPlaybackCount = 8;
        int fpsIdx = 0;
        if (tl->playbackMode() == engine::PlaybackMode::Dynamic) {
            fpsIdx = 0;
        } else if (tl->playbackMode() == engine::PlaybackMode::RefreshSync ||
                   tl->playbackFps() <= 0.0f) {
            fpsIdx = 1;
        } else {
            // 自定义帧率（设置卡片里拉过滑块）→ 显示最接近的预设项
            float best = 1e9f;
            for (int i = 2; i < kPlaybackCount; ++i) {
                const float d = std::fabs(tl->playbackFps() - kPlaybackFps[i]);
                if (d < best) {
                    best = d;
                    fpsIdx = i;
                }
            }
        }
        std::string fpsItems;
        for (const char* n : kPlaybackModes) {
            fpsItems += n;
            fpsItems.push_back('\0');
        }
        fpsItems.push_back('\0');
        if (aurora::comboRow(tr("播放帧率"), &fpsIdx, fpsItems.c_str(),
                             tr("动态 FPS = 跟随真实渲染帧率（受目标帧率上限约束）；"
                                "刷新率同步 = 每个渲染帧一帧，最顺滑"))) {
            if (fpsIdx == 0) {
                tl->setPlaybackMode(engine::PlaybackMode::Dynamic);
            } else if (fpsIdx == 1) {
                tl->setPlaybackMode(engine::PlaybackMode::RefreshSync);
            } else {
                tl->setPlaybackFps(kPlaybackFps[std::clamp(fpsIdx, 2, kPlaybackCount - 1)]);
            }
            // 记忆到 settings.json（load → 改 → save：不持有第二份设置状态）。
            persist::Settings settings;
            auto loaded = persist::SettingsStore::load();
            if (loaded) {
                settings = *loaded;
            }
            switch (tl->playbackMode()) {
            case engine::PlaybackMode::Dynamic:
                settings.playbackMode = "dynamic";
                break;
            case engine::PlaybackMode::FixedFps:
                settings.playbackMode = "fixed";
                break;
            default:
                settings.playbackMode = "refresh";
                break;
            }
            settings.playbackFps = static_cast<int>(std::lround(
                tl->playbackMode() == engine::PlaybackMode::FixedFps ? tl->playbackFps()
                                                                     : tl->playbackTargetFps()));
            settings.playbackSpeed = tl->speed();
            settings.playbackMaxCatchUp = tl->playbackMaxCatchUp();
            (void)persist::SettingsStore::save(settings);
        }
        char rate[128];
        if (tl->playbackMode() == engine::PlaybackMode::Dynamic) {
            if (tl->playbackTargetFps() <= 0.0f) {
                std::snprintf(rate, sizeof(rate), "%s × %.2f · %s · %s %.0f FPS", tr("动态"),
                              static_cast<double>(tl->speed()), tr("目标 跟随渲染"),
                              tr("实际"), static_cast<double>(tl->effectiveFps()));
            } else {
                std::snprintf(rate, sizeof(rate), "%s × %.2f · %s %d FPS · %s %.0f FPS",
                              tr("动态"), static_cast<double>(tl->speed()), tr("目标"),
                              static_cast<int>(std::lround(tl->playbackTargetFps())),
                              tr("实际"), static_cast<double>(tl->effectiveFps()));
            }
        } else if (tl->playbackMode() == engine::PlaybackMode::FixedFps &&
                   tl->playbackFps() > 0.0f) {
            std::snprintf(rate, sizeof(rate), "%s %.0f × %.2f = %.0f FPS", tr("实际推进"),
                          static_cast<double>(tl->playbackFps()),
                          static_cast<double>(tl->speed()),
                          static_cast<double>(tl->playbackFps() * tl->speed()));
        } else {
            std::snprintf(rate, sizeof(rate), "%s × %.2f", tr("刷新率同步"),
                          static_cast<double>(tl->speed()));
        }
        ImGui::TextDisabled("%s", rate);
        bool loop = tl->loop();
        if (aurora::toggleRow(tr("循环播放"), &loop)) {
            tl->toggleLoop(loop);
        }

        // —— 帧范围（In/Out，1 基显示）——
        aurora::sectionLabel(tr("帧范围"), tr("只播放轨迹的一段"));
        bool inOut = tl->inOutEnabled();
        if (aurora::toggleRow(tr("启用 In/Out"), &inOut)) {
            tl->setInOutEnabled(inOut);
        }
        ImGui::BeginDisabled(!inOut);
        int inI = static_cast<int>(std::min(tl->inFrame(), static_cast<uint64_t>(last))) + 1;
        int outI = static_cast<int>(std::min(tl->outFrame(), static_cast<uint64_t>(last))) + 1;
        bool changed = false;
        ImGui::SetNextItemWidth(110.0f);
        changed |= ImGui::InputInt(tr("起"), &inI);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        changed |= ImGui::InputInt(tr("止"), &outI);
        if (changed) {
            const int a = std::clamp(inI - 1, 0, last);
            const int b = std::clamp(outI - 1, 0, last);
            tl->setInOut(static_cast<uint64_t>(a), static_cast<uint64_t>(b));
        }
        ImGui::EndDisabled();
        char range[96];
        std::snprintf(range, sizeof(range), "%s %d – %d (%s)", tr("选区"), inI, outI,
                      tr("按帧号，1 基"));
        ImGui::TextDisabled("%s", range);
    }

private:
    float speed_ = 1.0f;
    float lastSpeed_ = -1.0f;
};
// 7. 分析（Analysis）—— 场景统计 + 逐分子统计（只读）
// =============================================================================
// 设计要点（2026-10-22 用户反馈）：
//   · 2042 个分子不能一次性全部展开——逐分子列表默认折叠，内部用 clipper 只画
//     可见行，并支持搜索 / 只看有轨迹的分子；
//   · 先给"能一眼看完"的汇总（总览 + 体系统计 + Rg/RMSE/RMSF 的均值/极值），
//     详细表按需展开；
//   · 统计结果可整表导出为 CSV（Excel 直接打开）或真正的 .xlsx 工作簿；
//   · 统计基于轨迹**第 0 帧**为参考（内存中的当前帧不影响结果），长轨迹按
//     200 帧等间隔抽样，界面上标明实际统计帧数。
// =============================================================================
class AnalysisModule : public IInspectorModule {
public:
    const char* id() const override { return "analysis"; }
    const char* label() const override { return "分析"; }
    const char* subtitle() const override { return "场景统计与结构信息"; }

    void onActivate(Workbench& wb) override {
        ensureStats(wb, false);
    }

    void render(Workbench& wb) override {
        DataManager* dm = wb.data();
        entity::SceneStructure* scene = wb.scene();
        if (dm == nullptr || scene == nullptr) {
            ImGui::TextDisabled("%s", tr("数据未就绪"));
            return;
        }
        // 首次进入/数据形状变化（分子数、体系数、帧数）时自动算一次；播放时
        // 场景 serial 每帧变，但这里**不**看 serial，避免播放中反复重算。
        const uint64_t sig = sceneSignature(scene);
        if (!computed_ || sig != lastSignature_) {
            ensureStats(wb, true);
        }

        renderOverview(wb);
        renderSystemsTable();
        renderActions(wb);
        renderPerMolecule();
        renderQuantumChemistry();
    }

private:
    static uint64_t sceneSignature(const entity::SceneStructure* scene) {
        if (scene == nullptr) {
            return 0;
        }
        uint64_t maxFrames = 0;
        for (const auto& inst : scene->instances()) {
            if (inst.trajectory) {
                maxFrames =
                    std::max<uint64_t>(maxFrames, inst.trajectory->frames.size());
            }
        }
        uint64_t sig = 1469598103934665603ull;
        const uint64_t parts[4] = {
            static_cast<uint64_t>(scene->instances().size()),
            static_cast<uint64_t>(scene->systems().size()),
            maxFrames,
            static_cast<uint64_t>(scene->coordsSoA().positions.size()),
        };
        for (uint64_t v : parts) {
            sig ^= v;
            sig *= 1099511628211ull;
        }
        return sig;
    }

    void ensureStats(Workbench& wb, bool force) {
        entity::SceneStructure* scene = wb.scene();
        if (scene == nullptr) {
            stats_ = MoleculeStats{};
            computed_ = false;
            return;
        }
        const uint64_t sig = sceneSignature(scene);
        if (!force && computed_ && sig == lastSignature_) {
            return;
        }
        stats_ = computeMoleculeStats(*scene);
        lastSignature_ = sig;
        computed_ = true;
    }

    void renderOverview(Workbench& wb) {
        const auto& s = stats_.summary;
        aurora::sectionLabel(tr("总览"));
        aurora::kvRow(tr("分子数"), std::to_string(s.molecules).c_str());
        aurora::kvRow(tr("体系数"), std::to_string(s.systems).c_str());
        aurora::kvRow(tr("原子数"), std::to_string(s.atoms).c_str());
        aurora::kvRow(tr("键数"), std::to_string(s.bonds).c_str());
        aurora::kvRow(tr("最大帧数"), std::to_string(s.maxFrames).c_str());
        aurora::kvRow(tr("表示数"),
                      std::to_string(wb.representations().size()).c_str());

        aurora::spacer(6.0f);
        aurora::sectionLabel(tr("结构统计"));
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.1f", s.totalMass);
        aurora::kvRow(tr("总质量 (u)"), buf);
        std::snprintf(buf, sizeof(buf), "%.1f", s.meanAtoms);
        aurora::kvRow(tr("平均原子数 / 分子"), buf);
        std::snprintf(buf, sizeof(buf), "%.1f", s.meanBonds);
        aurora::kvRow(tr("平均键数 / 分子"), buf);
        std::snprintf(buf, sizeof(buf), "%zu", s.withTrajectory);
        aurora::kvRow(tr("含轨迹分子数"), buf);

        aurora::spacer(6.0f);
        aurora::sectionLabel(tr("轨迹统计（以第 0 帧为参考）"));
        std::snprintf(buf, sizeof(buf), "%.3f", s.meanRadiusOfGyration);
        aurora::kvRow(tr("平均回转半径 Rg (Å)"), buf);
        std::snprintf(buf, sizeof(buf), "%.3f", s.maxRadiusOfGyration);
        aurora::kvRow(tr("最大回转半径 Rg (Å)"), buf);
        std::snprintf(buf, sizeof(buf), "%.4f", s.meanRmse);
        aurora::kvRow(tr("平均 RMSE (Å)"), buf);
        std::snprintf(buf, sizeof(buf), "%.4f", s.maxRmse);
        aurora::kvRow(tr("最大 RMSE (Å)"), buf);
        if (!s.maxRmseName.empty()) {
            std::string name = s.maxRmseName;
            if (name.size() > 26) {
                name = name.substr(0, 25) + "…";
            }
            aurora::kvRow(tr("最大 RMSE 分子"), name.c_str());
        }
        std::snprintf(buf, sizeof(buf), "%.4f", s.meanRmsf);
        aurora::kvRow(tr("平均 RMSF (Å)"), buf);
        aurora::helpTip(tr("RMSE 为轨迹相对首帧的坐标均方根误差（未做叠合/最优旋转）；"
                           "RMSF 为原子相对自身时间平均位置的涨落。长轨迹按 200 帧"
                           "等间隔抽样，逐分子表中的「统计帧数」是实际参与计算的帧数。"));
    }

    struct SystemAgg {
        std::string name;
        size_t molecules = 0;
        size_t atoms = 0;
        size_t bonds = 0;
        uint64_t maxFrames = 0;
        double rgSum = 0.0;
        double rmseSum = 0.0;
    };

    void renderSystemsTable() {
        if (stats_.molecules.empty()) {
            return;
        }
        std::vector<SystemAgg> aggs;
        auto findAgg = [&aggs](const std::string& name) -> SystemAgg& {
            for (auto& a : aggs) {
                if (a.name == name) {
                    return a;
                }
            }
            aggs.push_back(SystemAgg{});
            aggs.back().name = name;
            return aggs.back();
        };
        for (const auto& m : stats_.molecules) {
            SystemAgg& a = findAgg(m.system);
            ++a.molecules;
            a.atoms += m.atoms;
            a.bonds += m.bonds;
            a.maxFrames = std::max(a.maxFrames, m.frames);
            a.rgSum += m.radiusOfGyration;
            a.rmseSum += m.rmse;
        }
        if (aggs.size() <= 1) {
            return;   // 单体系时上面的总览已经给出同样信息，不再重复
        }
        aurora::spacer(6.0f);
        aurora::sectionLabel(tr("体系统计"));
        if (ImGui::BeginTable("##analysis_systems", 6,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn(tr("体系"));
            ImGui::TableSetupColumn(tr("分子"));
            ImGui::TableSetupColumn(tr("原子"));
            ImGui::TableSetupColumn(tr("键"));
            ImGui::TableSetupColumn("Rg (Å)");
            ImGui::TableSetupColumn("RMSE (Å)");
            ImGui::TableHeadersRow();
            for (const auto& a : aggs) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(a.name.empty() ? tr("未归组") : a.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", a.molecules);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", a.atoms);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%zu", a.bonds);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.2f", a.molecules > 0 ? a.rgSum / a.molecules : 0.0);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.3f", a.molecules > 0 ? a.rmseSum / a.molecules : 0.0);
            }
            ImGui::EndTable();
        }
    }

    void renderActions(Workbench& wb) {
        aurora::spacer(6.0f);
        if (aurora::chip(tr("重新计算"), false,
                         tr("按当前场景重新统计（轨迹涨落按 200 帧抽样）"))) {
            ensureStats(wb, true);
            wb.notify(tr("统计已更新"));
        }
        ImGui::SameLine(0, 6);
        if (aurora::chip(tr("复制统计"), false, tr("把总览文本复制到剪贴板"))) {
            ImGui::SetClipboardText(summaryText().c_str());
            wb.notify(tr("统计已复制"));
        }
        ImGui::SameLine(0, 6);
        if (aurora::chip(tr("导出 CSV"), false,
                         tr("导出全部分子统计，Excel 可直接打开"))) {
            exportCsv(wb);
        }
        ImGui::SameLine(0, 6);
        if (aurora::chip(tr("导出 Excel"), false,
                         tr("导出全部分子统计为 .xlsx 工作簿"))) {
            exportXlsx(wb);
        }
    }

    std::string summaryText() const {
        const auto& s = stats_.summary;
        char buf[1024];
        std::snprintf(
            buf, sizeof(buf),
            "MolecuLens 场景统计\n分子 %zu · 体系 %zu · 原子 %zu · 键 %zu · 帧 %zu\n"
            "总质量 %.1f u · 平均 Rg %.3f Å · 最大 Rg %.3f Å\n"
            "平均 RMSE %.4f Å · 最大 RMSE %.4f Å · 平均 RMSF %.4f Å",
            s.molecules, s.systems, s.atoms, s.bonds, s.maxFrames, s.totalMass,
            s.meanRadiusOfGyration, s.maxRadiusOfGyration, s.meanRmse, s.maxRmse,
            s.meanRmsf);
        return buf;
    }

    static bool containsCaseInsensitive(const std::string& haystack,
                                        const std::string& needle) {
        if (needle.empty()) {
            return true;
        }
        if (needle.size() > haystack.size()) {
            return false;
        }
        for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
            bool match = true;
            for (size_t k = 0; k < needle.size(); ++k) {
                const auto a = static_cast<unsigned char>(haystack[i + k]);
                const auto b = static_cast<unsigned char>(needle[k]);
                if (std::tolower(a) != std::tolower(b)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    }

    void renderPerMolecule() {
        aurora::spacer(6.0f);
        const std::string header =
            std::string(tr("逐分子统计")) + " (" +
            std::to_string(stats_.molecules.size()) + ")";
        if (!ImGui::CollapsingHeader(header.c_str())) {
            return;
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##analysis_filter", tr("按分子名 / 体系名搜索"),
                                 filter_, sizeof(filter_));
        ImGui::Checkbox(tr("只显示有轨迹的分子"), &onlyTrajectory_);
        const auto rows = visibleRows();
        ImGui::SameLine();
        ImGui::TextDisabled("%s: %zu", tr("匹配"), rows.size());
        if (stats_.molecules.empty()) {
            ImGui::TextDisabled("%s", tr("场景为空"));
            return;
        }

        constexpr float kTableH = 300.0f;
        // 检查器只有 ~320 px 宽：只放最关键的 6 列（分子/原子/帧/Rg/RMSE/RMSF），
        // 体系、键数、统计帧数、最大位移放进行 tooltip；完整字段仍在 CSV/Excel 里。
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("##analysis_molecules", 6, flags,
                              ImVec2(0.0f, kTableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("分子"), ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn(tr("原子"), ImGuiTableColumnFlags_WidthFixed, 38.0f);
            ImGui::TableSetupColumn(tr("帧"), ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("Rg", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("RMSE", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupColumn("RMSF", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    const MoleculeStat& m =
                        stats_.molecules[rows[static_cast<size_t>(r)]];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(m.name.empty() ? tr("未命名")
                                                          : m.name.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "%s: %s\n%s: %zu\n%s: %u\n%s: %.3f Å",
                            tr("体系"),
                            m.system.empty() ? tr("未归组") : m.system.c_str(),
                            tr("键"), m.bonds, tr("统计帧数"), m.framesUsed,
                            tr("最大位移"), m.maxDisplacement);
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%zu", m.atoms);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%llu", static_cast<unsigned long long>(m.frames));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.2f", m.radiusOfGyration);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.3f", m.rmse);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.3f", m.rmsf);
                }
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%s", tr("表内滚动查看全部分子；导出按钮包含全部行（不受搜索/筛选影响）"));
    }

    std::vector<size_t> visibleRows() const {
        std::vector<size_t> out;
        out.reserve(stats_.molecules.size());
        const std::string needle(filter_);
        for (size_t i = 0; i < stats_.molecules.size(); ++i) {
            const auto& m = stats_.molecules[i];
            if (onlyTrajectory_ && m.frames <= 1) {
                continue;   // 单帧 = 静态结构，不算"有轨迹"
            }
            if (!containsCaseInsensitive(m.name, needle) &&
                !containsCaseInsensitive(m.system, needle)) {
                continue;
            }
            out.push_back(i);
        }
        return out;
    }

    static std::string csvEscape(const std::string& in) {
        if (in.find_first_of(",\"\n\r") == std::string::npos) {
            return in;
        }
        std::string out = "\"";
        for (char c : in) {
            if (c == '"') {
                out += "\"\"";
            } else {
                out.push_back(c);
            }
        }
        out += "\"";
        return out;
    }

    void exportCsv(Workbench& wb) {
        const std::string filter =
            trItems({"CSV 表格 (*.csv)", "*.csv", "所有文件 (*.*)", "*.*"});
        const std::string path =
            wb.chooseSaveFile("molecule_stats.csv", filter.c_str(), "csv");
        if (path.empty()) {
            return;
        }
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) {
            wb.notify(tr("导出失败：无法创建文件"));
            return;
        }
        const auto headers = moleculeStatsCsvHeader();
        const auto rows = moleculeStatsCsvRows(stats_);
        f << "\xEF\xBB\xBF";   // UTF-8 BOM：Excel 双击打开不乱码
        for (size_t i = 0; i < headers.size(); ++i) {
            if (i > 0) {
                f << ',';
            }
            f << csvEscape(headers[i]);
        }
        f << "\r\n";
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) {
                    f << ',';
                }
                f << csvEscape(row[i]);
            }
            f << "\r\n";
        }
        if (!f.good()) {
            wb.notify(tr("导出失败：写入不完整"));
            return;
        }
        wb.notify(tr("已导出 CSV：") + path);
    }

    void exportXlsx(Workbench& wb) {
        const std::string filter =
            trItems({"Excel 工作簿 (*.xlsx)", "*.xlsx", "所有文件 (*.*)", "*.*"});
        const std::string path =
            wb.chooseSaveFile("molecule_stats.xlsx", filter.c_str(), "xlsx");
        if (path.empty()) {
            return;
        }
        const auto headers = moleculeStatsCsvHeader();
        std::vector<std::vector<xlsx::Cell>> rows;
        rows.reserve(stats_.molecules.size());
        for (size_t i = 0; i < stats_.molecules.size(); ++i) {
            const auto& m = stats_.molecules[i];
            std::vector<xlsx::Cell> row;
            row.reserve(headers.size());
            row.push_back(xlsx::Cell::numberCell(static_cast<double>(i + 1)));
            row.push_back(xlsx::Cell::textCell(m.name));
            row.push_back(xlsx::Cell::textCell(m.system));
            row.push_back(xlsx::Cell::numberCell(static_cast<double>(m.atoms)));
            row.push_back(xlsx::Cell::numberCell(static_cast<double>(m.bonds)));
            row.push_back(xlsx::Cell::numberCell(static_cast<double>(m.frames)));
            row.push_back(xlsx::Cell::numberCell(static_cast<double>(m.framesUsed)));
            row.push_back(xlsx::Cell::numberCell(m.mass));
            row.push_back(xlsx::Cell::numberCell(m.centroid.x));
            row.push_back(xlsx::Cell::numberCell(m.centroid.y));
            row.push_back(xlsx::Cell::numberCell(m.centroid.z));
            row.push_back(xlsx::Cell::numberCell(m.radiusOfGyration));
            row.push_back(xlsx::Cell::numberCell(m.extent.x));
            row.push_back(xlsx::Cell::numberCell(m.extent.y));
            row.push_back(xlsx::Cell::numberCell(m.extent.z));
            row.push_back(xlsx::Cell::numberCell(m.rmse));
            row.push_back(xlsx::Cell::numberCell(m.rmsf));
            row.push_back(xlsx::Cell::numberCell(m.maxDisplacement));
            rows.push_back(std::move(row));
        }
        std::string err;
        if (!xlsx::writeSheet(path, tr("逐分子统计"), headers, rows, &err)) {
            wb.notify(tr("导出 Excel 失败：") + err);
            return;
        }
        wb.notify(tr("已导出 Excel：") + path);
    }

    static void renderQuantumChemistry() {
        aurora::spacer(6.0f);
        aurora::sectionLabel(tr("量子化学分析"));
        ImGui::BeginDisabled();
        bool dummy = false;
        ImGui::Checkbox(tr("电荷与键级（Multiwfn 输出）"), &dummy);
        ImGui::Checkbox(tr("NBO 二阶微扰 (E2)"), &dummy);
        ImGui::Checkbox(tr("IGMH / IRI 相互作用图"), &dummy);
        ImGui::EndDisabled();
        aurora::helpTip(tr("这些分析需要量子化学输出文件（.fchk/.log），"
                           "当前版本尚未接入；ESP 与轨道等值面请用「ESP / 轨道」面板。"));
    }

    MoleculeStats stats_;
    uint64_t lastSignature_ = 0;
    bool computed_ = false;
    bool onlyTrajectory_ = false;
    char filter_[128] = "";
};

// =============================================================================
// 8. 日志（Log）
// =============================================================================
class LogModule : public IInspectorModule {
public:
    const char* id() const override { return "log"; }
    const char* label() const override { return "日志"; }
    const char* subtitle() const override { return "运行时消息"; }

    void render(Workbench& wb) override {
        if (aurora::chip(tr("清空"), false)) {
            wb.clearLog();
        }
        ImGui::SameLine();
        ImGui::Checkbox(tr("自动滚动"), &autoScroll_);

        aurora::spacer(4.0f);
        const auto& lines = wb.logLines();
        ImGui::BeginChild("##log_scroll", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& line : lines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (autoScroll_ &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
        if (lines.empty()) {
            ImGui::TextDisabled("%s", tr("暂无消息"));
        }
        ImGui::EndChild();
    }

private:
    bool autoScroll_ = true;
};

// =============================================================================
// 9. 设置（Settings）
// =============================================================================
class SettingsModule : public IInspectorModule {
public:
    const char* id() const override { return "settings"; }
    const char* label() const override { return "设置"; }
    const char* subtitle() const override { return "外观、背景、光照与相机"; }

    // 背景预设色板（点击即写入渲染器）
    static void bgPresets(Workbench& wb) {
        struct Swatch {
            const char* name;
            ImVec4 c;
        };
        const Swatch swatches[] = {
            {"深灰", ImVec4(0.22f, 0.24f, 0.28f, 1.0f)},
            {"白", ImVec4(1.00f, 1.00f, 1.00f, 1.0f)},
            {"浅灰", ImVec4(0.93f, 0.95f, 0.97f, 1.0f)},
            {"深蓝", ImVec4(0.06f, 0.08f, 0.12f, 1.0f)},
            {"黑", ImVec4(0.00f, 0.00f, 0.00f, 1.0f)},
        };
        for (int i = 0; i < 5; ++i) {
            const Swatch& s = swatches[i];
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Border, aurora::palette().borderStrong);
            const bool clicked = ImGui::ColorButton(
                "##sw", s.c,
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                ImVec2(30, 30));
            ImGui::PopStyleColor();
            if (clicked) {
                if (wb.renderCtl() != nullptr) {
                    wb.renderCtl()->setBackgroundColor(s.c.x, s.c.y, s.c.z);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", s.name);
            }
            ImGui::PopID();
            if (i < 4) {
                ImGui::SameLine(0, 6);
            }
        }
    }

    void render(Workbench& wb) override {
        auto* rc = wb.renderCtl();
        auto* scene = wb.scene();
        const entity::SceneProperties* props =
            scene != nullptr ? &scene->properties() : nullptr;

        // —— 外观 ——
        if (aurora::cardBegin("##set_look", tr("外观"), tr("主题、检查器宽度"))) {
            if (aurora::chip(aurora::isDark() ? tr("切换到浅色") : tr("切换到深色"),
                             aurora::isDark(), tr("浅色为默认；切换会记住到设置文件"))) {
                wb.toggleTheme();
            }
            aurora::spacer(4.0f);
            aurora::sectionLabel(tr("检查器宽度"), tr("260–420 px，越大视口越窄"));
            float w = wb.inspectorWidth();
            if (aurora::sliderRow(tr("宽度"), &w, aurora::metrics::kInspectorMin,
                                  aurora::metrics::kInspectorMax, "%.0f px")) {
                wb.setInspectorWidth(w);
            }
            if (aurora::chip(tr("恢复默认宽度"), false)) {
                wb.setInspectorWidth(aurora::metrics::kInspector);
            }
        }
        aurora::cardEnd();

        // —— 性能（垂直同步）——
        if (aurora::cardBegin("##set_perf", tr("性能"),
                              tr("垂直同步会影响帧率上限与画面稳定性"))) {
            static bool vsyncOn = true;
            static bool vsyncInited = false;
            if (!vsyncInited) {
                auto loaded = persist::SettingsStore::load();
                if (loaded) {
                    vsyncOn = loaded.value().vsync;
                }
                vsyncInited = true;
            }
            if (aurora::chip(vsyncOn ? tr("垂直同步：开") : tr("垂直同步：关"), vsyncOn,
                             tr("开 = FIFO，画面最稳、帧率跟显示器刷新率；"
                                "关 = 不等待 vblank，帧率不受限但可能撕裂。改动下次启动生效"))) {
                vsyncOn = !vsyncOn;
                persist::Settings s;
                auto loaded = persist::SettingsStore::load();
                if (loaded) {
                    s = loaded.value();
                }
                s.vsync = vsyncOn;
                (void)persist::SettingsStore::save(s);
            }
        }
        aurora::cardEnd();

        // —— 轨迹播放（动态 FPS，v2.19）——
        // 播放节奏以 TimelineController 为唯一事实源（构造时已应用 settings.json）：
        // 这里只做"读当前值 → 改控制器 → 落盘"，不另存一份界面状态。
        if (aurora::cardBegin("##set_playback", tr("轨迹播放"),
                              tr("播放节奏跟随真实渲染帧率，卡顿后不补一大段"))) {
            TimelineController* tl = wb.timeline();
            int pbMode = 0;           // 0 动态 / 1 固定 / 2 刷新同步
            int pbTargetFps = 60;     // 0 = 跟随渲染帧率
            float pbSpeed = 1.0f;
            int pbCatchUp = 2;
            if (tl != nullptr) {
                pbMode = tl->playbackMode() == engine::PlaybackMode::FixedFps
                             ? 1
                             : (tl->playbackMode() == engine::PlaybackMode::RefreshSync ? 2 : 0);
                pbTargetFps = std::clamp(
                    static_cast<int>(std::lround(pbMode == 1 ? tl->playbackFps()
                                                             : tl->playbackTargetFps())),
                    0, 144);
                pbSpeed = std::clamp(tl->speed(), 0.1f, 4.0f);
                pbCatchUp = tl->playbackMaxCatchUp();
            }
            auto persistPlayback = [&]() {
                persist::Settings s;
                auto loaded = persist::SettingsStore::load();
                if (loaded) {
                    s = *loaded;
                }
                s.playbackMode =
                    pbMode == 1 ? "fixed" : (pbMode == 2 ? "refresh" : "dynamic");
                s.playbackFps = pbTargetFps;
                s.playbackSpeed = pbSpeed;
                s.playbackMaxCatchUp = pbCatchUp;
                (void)persist::SettingsStore::save(s);
            };
            aurora::sectionLabel(tr("播放节奏"), tr("动态 = 跟随渲染帧率；固定 = 指定 FPS"));
            static const char* const kModeItems[] = {u8"动态", u8"固定", u8"同步"};
            if (aurora::segmented("##pb_mode", kModeItems, 3, &pbMode) && tl != nullptr) {
                if (pbMode == 1) {
                    tl->setPlaybackFps(pbTargetFps > 0 ? static_cast<float>(pbTargetFps)
                                                       : 60.0f);
                } else if (pbMode == 2) {
                    tl->setPlaybackMode(engine::PlaybackMode::RefreshSync);
                } else {
                    tl->setPlaybackMode(engine::PlaybackMode::Dynamic);
                    tl->setPlaybackTargetFps(static_cast<float>(pbTargetFps));
                }
                persistPlayback();
            }
            ImGui::BeginDisabled(pbMode == 2);
            aurora::sectionLabel(tr("目标帧率"), tr("0 = 跟随渲染帧率"));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##pb_fps", &pbTargetFps, 0, 144)) {
                if (tl != nullptr) {
                    if (pbMode == 1) {
                        tl->setPlaybackFps(pbTargetFps > 0
                                               ? static_cast<float>(pbTargetFps)
                                               : 60.0f);
                    } else {
                        tl->setPlaybackTargetFps(static_cast<float>(pbTargetFps));
                    }
                }
                persistPlayback();
            }
            ImGui::EndDisabled();
            if (aurora::stepper(tr("追帧上限"), &pbCatchUp, 1, 1, 8,
                                tr("卡顿后每渲染帧最多补几帧；多余时间预算直接丢弃"))) {
                if (tl != nullptr) {
                    tl->setPlaybackMaxCatchUp(pbCatchUp);
                }
                persistPlayback();
            }
            if (aurora::sliderRow(tr("播放倍速"), &pbSpeed, 0.1f, 4.0f, "%.2f×")) {
                if (tl != nullptr) {
                    tl->setSpeed(pbSpeed);
                }
                persistPlayback();
            }
            char status[128];
            const float eff = tl != nullptr ? tl->effectiveFps() : 0.0f;
            if (pbMode == 0) {
                if (pbTargetFps <= 0) {
                    std::snprintf(status, sizeof(status), "%s × %.2f · %s · %s %.0f FPS",
                                  tr("动态"), static_cast<double>(pbSpeed),
                                  tr("目标 跟随渲染"), tr("实际"),
                                  static_cast<double>(eff));
                } else {
                    std::snprintf(status, sizeof(status),
                                  "%s × %.2f · %s %d FPS · %s %.0f FPS", tr("动态"),
                                  static_cast<double>(pbSpeed), tr("目标"), pbTargetFps,
                                  tr("实际"), static_cast<double>(eff));
                }
            } else if (pbMode == 1) {
                std::snprintf(status, sizeof(status), "%s %.0f × %.2f = %.0f FPS",
                              tr("实际推进"), static_cast<double>(pbTargetFps),
                              static_cast<double>(pbSpeed),
                              static_cast<double>(pbTargetFps * pbSpeed));
            } else {
                std::snprintf(status, sizeof(status), "%s × %.2f · %s %.0f FPS",
                              tr("刷新率同步"), static_cast<double>(pbSpeed), tr("实际"),
                              static_cast<double>(eff));
            }
            ImGui::TextDisabled("%s", status);
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().textFaint);
            ImGui::TextWrapped("%s",
                               tr("动态 FPS 用渲染 dt 的平滑值决定播放间隔：120Hz 屏 + 目标"
                                  " 60 时每两个渲染帧推进一帧；目标 0 表示每个渲染帧一帧。"));
            ImGui::PopStyleColor();
        }
        aurora::cardEnd();

        // —— 背景（取色器）——
        if (aurora::cardBegin("##set_bg", tr("背景与主题色"),
                              tr("视口背景 + 整套 UI 的主色"))) {
            float bg[3] = {0.10f, 0.10f, 0.15f};
            if (props != nullptr) {
                bg[0] = props->bgColor[0];
                bg[1] = props->bgColor[1];
                bg[2] = props->bgColor[2];
            }
            aurora::sectionLabel(tr("自定义颜色"), tr("点击色块打开取色器（HSV 轮盘）"));
            if (ImGui::ColorEdit3("##bg_color", bg,
                                  ImGuiColorEditFlags_NoInputs |
                                      ImGuiColorEditFlags_PickerHueWheel)) {
                if (rc != nullptr) {
                    rc->setBackgroundColor(bg[0], bg[1], bg[2]);
                }
            }
            aurora::spacer(4.0f);
            aurora::sectionLabel(tr("常用背景"));
            bgPresets(wb);

            // —— 调色盘 · 主题强调色（与「原子颜色」分开的第二个组件）——
            aurora::spacer(8.0f);
            aurora::hline();
            aurora::sectionLabel(tr("主题强调色"), tr("按钮/选中态/滑块随主色变化"));
            ImVec4 acc = aurora::accentColor();
            float ac[3] = {acc.x, acc.y, acc.z};
            if (ImGui::ColorEdit3(
                    "##accent_color", ac,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
                const ImVec4 c(ac[0], ac[1], ac[2], 1.0f);
                aurora::setAccent(c);
                persistAccent(&c);
            }
            // 8 个预设主色：点击立即生效并写入 settings.json 的 accent（"#RRGGBB"）
            // 低饱和主色：避免 8 个高饱和色块把界面"调色板化"（用户反馈太杂）
            static const ImVec4 kAccents[8] = {
                ImVec4(0.145f, 0.388f, 0.922f, 1.0f),   // 蓝（默认）
                ImVec4(0.129f, 0.510f, 0.561f, 1.0f),   // 青灰
                ImVec4(0.180f, 0.541f, 0.353f, 1.0f),   // 松绿
                ImVec4(0.706f, 0.510f, 0.180f, 1.0f),   // 芥末
                ImVec4(0.757f, 0.353f, 0.180f, 1.0f),   // 陶土
                ImVec4(0.706f, 0.235f, 0.376f, 1.0f),   // 莓红
                ImVec4(0.478f, 0.333f, 0.706f, 1.0f),   // 灰紫
                ImVec4(0.333f, 0.396f, 0.478f, 1.0f),   // 石板
            };
            for (int i = 0; i < 8; ++i) {
                ImGui::PushID(1000 + i);
                if (ImGui::ColorButton(
                        "##acc", kAccents[i],
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                        ImVec2(24, 24))) {
                    aurora::setAccent(kAccents[i]);
                    persistAccent(&kAccents[i]);
                }
                ImGui::PopID();
                if (i < 7) {
                    ImGui::SameLine(0, 6);
                }
            }
            if (aurora::chip(tr("恢复默认主色"), false)) {
                aurora::clearAccent();
                persistAccent(nullptr);
            }
        }
        aurora::cardEnd();

        // —— 光照与阴影 ——
        if (aurora::cardBegin("##set_light", tr("光照与阴影"),
                              tr("半球环境光 + 方向光"))) {
            float ambient = props != nullptr ? props->ambient : 0.45f;
            if (aurora::sliderRow(tr("环境光"), &ambient, 0.0f, 1.0f, "%.2f")) {
                if (rc != nullptr) {
                    rc->setAmbient(ambient);
                }
            }
            float az = 0.0f;
            float el = 0.0f;
            if (rc != nullptr) {
                rc->dirLightAngles(&az, &el);
            }
            constexpr float kRad2Deg = 57.29578f;
            constexpr float kDeg2Rad = 0.01745329f;
            az *= kRad2Deg;
            el *= kRad2Deg;
            bool lightChanged = false;
        lightChanged |= aurora::sliderNum(tr("方位角"), &az, -180.0f, 180.0f, "%.0f°", 1.0f, tr("绕竖直轴旋转光源的方向"));
        lightChanged |= aurora::sliderNum(tr("仰角"), &el, 2.0f, 89.0f, "%.0f°", 1.0f, tr("光源与水平面的夹角，越接近 90° 越像顶光"));
            if (lightChanged && rc != nullptr) {
                rc->setDirLightAngles(az * kDeg2Rad, el * kDeg2Rad);
            }
            float intensity = props != nullptr ? props->dirLightIntensity : 0.35f;
        if (aurora::sliderNum(tr("方向光强度"), &intensity, 0.0f, 2.0f, "%.2f", 0.01f)) {
                if (rc != nullptr) {
                    rc->setDirLightIntensity(intensity);
                }
            }
            bool shadows = props != nullptr ? props->shadowsEnabled : true;
            if (aurora::toggleRow(tr("方向光阴影"), &shadows)) {
                if (rc != nullptr) {
                    rc->setShadowsEnabled(shadows);
                }
            }
            if (shadows) {
                float strength = props != nullptr ? props->shadowStrength : 0.65f;
        if (aurora::sliderNum(tr("阴影强度"), &strength, 0.0f, 1.0f, "%.2f", 0.01f, tr("球体阴影的浓度（屏幕空间环境光遮蔽类效果）"))) {
                    if (rc != nullptr) {
                        rc->setShadowStrength(strength);
                    }
                }
            }
        }
        aurora::cardEnd();

        // —— 渲染风格（v2.2：柔和光 / 轮廓 / 出版出图）——
        if (aurora::cardBegin("##set_style", tr("渲染风格"),
                              tr("柔和棚拍光 + 轮廓描边，可一键出版图"))) {
            if (rc != nullptr) {
                // ImGui::Combo 的 items 是 '\0' 分隔串；字面量末尾自带一个 '\0'。
                static const char* kStyleItems = "默认\0柔和\0出版\0鲜明\0";
                int preset = static_cast<int>(rc->stylePreset());
                if (aurora::comboRow(tr("预设"), &preset, kStyleItems,
                                     tr("出版 = 白底 + 高调柔光 + 关阴影 + 强轮廓"))) {
                    rc->applyRenderStylePreset(
                        static_cast<engine::RenderStylePreset>(preset));
                }
                engine::RenderStyle st = rc->renderStyle();
                bool changed = false;
                changed |= aurora::sliderRow(
                    tr("漫反射环绕"), &st.wrap, 0.0f, 0.8f, "%.2f",
                    tr("0 = 硬明暗交界；越大过渡越柔和"));
                changed |= aurora::sliderRow(
                    tr("轮廓描边"), &st.outline, 0.0f, 1.0f, "%.2f",
                    tr("掠射角压暗，替代后处理描边"));
                changed |= aurora::sliderRow(tr("边缘补光"), &st.rim, 0.0f, 0.5f,
                                             "%.2f");
                changed |= aurora::sliderRow(
                    tr("距离淡出"), &st.depthCue, 0.0f, 1.0f, "%.2f",
                    tr("需要先开启雾；淡出目标 = 背景色"));
                if (changed) {
                    rc->setRenderStyle(st);
                }
            }
        }
        aurora::cardEnd();

        // —— 剖切面（v2.3）——
        if (aurora::cardBegin("##set_clip", tr("剖切面"),
                              tr("沿轴切开分子/表面，观察内部"))) {
            if (rc != nullptr) {
                bool clipOn = props != nullptr ? props->clipEnabled : false;
                if (aurora::toggleRow(tr("启用剖切"), &clipOn,
                                      tr("在片元着色器丢弃平面正侧"))) {
                    rc->setClipEnabled(clipOn);
                }
                bool on = false;
                glm::vec3 n(1.0f, 0.0f, 0.0f);
                float off = 0.0f;
                rc->clipState(&on, &n, &off);
                int axis = 1;   // 默认 +X
                if (n.x < -0.5f) {
                    axis = 0;
                } else if (n.x > 0.5f) {
                    axis = 1;
                } else if (n.y < -0.5f) {
                    axis = 2;
                } else if (n.y > 0.5f) {
                    axis = 3;
                } else if (n.z < -0.5f) {
                    axis = 4;
                } else {
                    axis = 5;
                }
                if (aurora::comboRow(tr("方向"), &axis,
                                     "-X\0+X\0-Y\0+Y\0-Z\0+Z\0",
                                     tr("法线指向被切掉的一侧"))) {
                    rc->setClipAxis(axis, off);
                }
                float o = off;
                if (aurora::sliderRow(tr("切面位置"), &o, -60.0f, 60.0f, "%.1f Å",
                                      tr("沿法线方向移动平面（保留 <= 位置）"))) {
                    rc->setClipOffset(o);
                }
                ImGui::TextDisabled("%s",
                                    tr("提示：剖面只切球/柱/网格；线框与坐标轴不裁剪，"
                                       "切面本身不填充。"));
            }
        }
        aurora::cardEnd();

        // —— 相机 ——
        if (aurora::cardBegin("##set_camera", tr("相机"), nullptr)) {
            bool ortho = ortho_;
            if (aurora::toggleRow(tr("正交投影"), &ortho)) {
                ortho_ = ortho;
                if (wb.cameraCtl() != nullptr) {
                    wb.cameraCtl()->setPerspective(ortho);
                }
            }
        if (aurora::sliderNum(tr("视野 FOV"), &fov_, 15.0f, 90.0f, "%.0f°", 1.0f, tr("视角越小越接近长焦，透视压缩感更强"))) {
                if (wb.cameraCtl() != nullptr) {
                    wb.cameraCtl()->setFov(fov_);
                }
            }
            if (aurora::chip(tr("取景全部"), false)) {
                wb.frameCamera();
            }
            ImGui::SameLine();
            if (aurora::chip(tr("重置视角"), false)) {
                wb.resetCamera();
            }
        }
        aurora::cardEnd();

        // —— 语言与关于 ——
        if (aurora::cardBegin("##set_about", tr("语言与关于"), nullptr)) {
            int lang = i18n::current() == i18n::Language::En ? 1 : 0;
            const char* langs = "简体中文\0English\0";
            aurora::sectionLabel(tr("界面语言"));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##lang", &lang, langs)) {
                i18n::setLanguage(lang == 1 ? i18n::Language::En
                                            : i18n::Language::Zh);
            }
            aurora::spacer(6.0f);
            aurora::kvRow("MolecuLens", "v2.0 · Aurora");
            aurora::kvRow(tr("渲染后端"), "Vulkan 1.3");
            aurora::kvRow(tr("界面"), "Dear ImGui 1.91");
            aurora::kvRow(tr("检查器"),
                          (std::to_string(wb.moduleCount()) + " " + tr("个模块")).c_str());
        }
        aurora::cardEnd();
    }

private:
    bool ortho_ = false;
    float fov_ = 45.0f;
};

// =============================================================================
// 10. 标注（Annotation）—— 水盒 / 分子标识 / 标签 / 箭头 / 出版风格
// =============================================================================
class AnnotationModule : public IInspectorModule {
public:
    const char* id() const override { return "annotation"; }
    const char* label() const override { return "标注"; }
    const char* subtitle() const override { return "水盒、分子标识、箭头与出版风格"; }

    void render(Workbench& wb) override {
        // —— 水盒 ——
        if (aurora::cardBegin("##ann_box", tr("水盒 / 周期盒"),
                              tr("跟随轨迹盒，或手动指定位置与尺寸"))) {
            bool on = wb.showBox();
            if (aurora::toggleRow(tr("显示水盒"), &on,
                                  tr("把盒子的 12 条棱画在场景上；导出 PNG 时一并画出"))) {
                wb.setShowBox(on);
            }
            int mode = wb.boxManual() ? 1 : 0;
            if (ImGui::RadioButton(tr("跟随轨迹盒"), &mode, 0)) {
                wb.setBoxManual(false);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(tr("手动指定"), &mode, 1)) {
                wb.setBoxManual(true);
            }
            if (wb.boxManual()) {
                glm::vec3 c = wb.boxCenter();
                float cc[3] = {c.x, c.y, c.z};
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat3(tr("盒中心 (Å)"), cc, 0.05f, -1e4f, 1e4f, "%.2f")) {
                    wb.setBoxCenter(glm::vec3(cc[0], cc[1], cc[2]));
                }
                glm::vec3 h = wb.boxHalf();
                float hh[3] = {h.x, h.y, h.z};
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat3(tr("盒半长 (Å)"), hh, 0.05f, 0.1f, 1e4f, "%.2f")) {
                    wb.setBoxHalf(glm::vec3(hh[0], hh[1], hh[2]));
                }
                if (aurora::chip(tr("适配场景"), false,
                                 tr("按当前场景包围盒自动填中心与半长（±5% 余量）"))) {
                    wb.fitBoxToScene();
                }
            }
            ImGui::TextColored(aurora::palette().textMuted, "%s", tr("盒线颜色"));
            ImGui::SameLine();
            ImVec4 bc = wb.boxColor();
            if (ImGui::ColorEdit4("##box_color", &bc.x, ImGuiColorEditFlags_NoInputs)) {
                wb.setBoxColor(bc);
            }
            float lw = wb.boxLineWidth();
            if (aurora::sliderRow(tr("线宽"), &lw, 0.8f, 4.0f, "%.1f")) {
                wb.setBoxLineWidth(lw);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().textFaint);
            ImGui::TextWrapped("%s", tr("«跟随轨迹盒» 需要轨迹首帧带 box（DCD/GRO/XTC）；"
                                        "纯 PDB 请用手动指定。"));
            ImGui::PopStyleColor();
        }
        aurora::cardEnd();

        // —— 标签 ——
        if (aurora::cardBegin("##ann_label", tr("文字标签"),
                              tr("锚在分子质心、任意坐标或视图中心"))) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##label_text", tr("标签文字，如 LH-I / B850 / PA"),
                                     text_, sizeof(text_));
            aurora::sectionLabel(tr("锚点"));
            ImGui::RadioButton(tr("分子序号"), &labelAnchor_, 0);
            ImGui::SameLine();
            ImGui::RadioButton(tr("坐标"), &labelAnchor_, 1);
            ImGui::SameLine();
            ImGui::RadioButton(tr("视图中心"), &labelAnchor_, 2);
            if (labelAnchor_ == 0) {
                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputInt("##mol_index", &molIndex_);
            } else if (labelAnchor_ == 1) {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragFloat3(tr("世界坐标 (Å)"), labelPos_, 0.05f, -1e4f, 1e4f, "%.2f");
            }
            ImGui::TextColored(aurora::palette().textMuted, "%s", tr("颜色"));
            ImGui::SameLine();
            ImGui::ColorEdit4("##lcolor", &labelColor_.x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::SliderFloat("##lsize", &labelSize_, 9.0f, 48.0f, "%.0f px");
            if (aurora::chip(tr("添加标签"), true, tr("按上面的锚点加入一个文字标签"))) {
                glm::vec3 anchor = wb.viewCenter();
                if (labelAnchor_ == 0) {
                    anchor = wb.moleculeCenter(
                        static_cast<size_t>(std::max(0, molIndex_)));
                } else if (labelAnchor_ == 1) {
                    anchor = glm::vec3(labelPos_[0], labelPos_[1], labelPos_[2]);
                }
                wb.addLabel(text_[0] != '\0' ? text_ : "label", anchor);
                // 颜色/字号作用于刚加入的标签（addLabel 给的是默认值）
                if (!wb.labels().empty()) {
                    auto& last = wb.labels().back();
                    last.color = labelColor_;
                    last.size = labelSize_;
                }
            }

            ImGui::Spacing();
            aurora::sectionLabel(tr("批量分子标识"));
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputInt("##bulk", &bulkCount_);
            if (aurora::chip(tr("为前 N 个分子命名"), false,
                             tr("按分子名生成标签（大体系建议 < 50）"))) {
                wb.labelMolecules(0, static_cast<size_t>(std::max(0, bulkCount_)));
            }

            ImGui::Spacing();
            auto& labels = wb.labels();
            if (labels.empty()) {
                ImGui::TextDisabled("%s", tr("还没有标签"));
            }
            int removeIdx = -1;
            for (size_t i = 0; i < labels.size(); ++i) {
                auto& l = labels[i];
                ImGui::PushID(static_cast<int>(i));
                bool vis = l.visible;
                if (ImGui::Checkbox("##v", &vis)) {
                    l.visible = vis;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(l.text.c_str());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::SliderFloat("##s", &l.size, 9.0f, 48.0f, "%.0f");
                ImGui::SameLine();
                ImGui::ColorEdit4("##lc", &l.color.x, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("删除"))) {
                    removeIdx = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            if (removeIdx >= 0) {
                labels.erase(labels.begin() + removeIdx);
            }
        }
        aurora::cardEnd();

        // —— 箭头 ——
        if (aurora::cardBegin("##ann_arrow", tr("箭头"),
                              tr("连接两个分子/点；支持「光」波浪箭头"))) {
            aurora::sectionLabel(tr("端点锚定"));
            ImGui::RadioButton(tr("分子序号"), &arrowAnchor_, 0);
            ImGui::SameLine();
            ImGui::RadioButton(tr("坐标"), &arrowAnchor_, 1);
            if (arrowAnchor_ == 0) {
                ImGui::SetNextItemWidth(84.0f);
                ImGui::InputInt("##arrow_from", &arrowFrom_);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(84.0f);
                ImGui::InputInt("##arrow_to", &arrowTo_);
            } else {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragFloat3(tr("起点 (Å)"), arrowFromPos_, 0.05f, -1e4f, 1e4f, "%.2f");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::DragFloat3(tr("终点 (Å)"), arrowToPos_, 0.05f, -1e4f, 1e4f, "%.2f");
            }
            {
                const std::string styles = trItems({"普通箭头", "波浪（光）"});
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::Combo("##arrow_style", &arrowStyle_, styles.c_str());
            }
            float aw = arrowWidth_;
            if (aurora::sliderRow(tr("线宽"), &aw, 1.0f, 8.0f, "%.1f")) {
                arrowWidth_ = aw;
            }
            float ah = arrowHead_;
            if (aurora::sliderRow(tr("箭头大小"), &ah, 0.4f, 2.5f, "%.2f")) {
                arrowHead_ = ah;
            }
            ImGui::Checkbox(tr("双头箭头"), &arrowDouble_);
            ImGui::SameLine();
            ImGui::ColorEdit4("##acolor", &arrowColor_.x, ImGuiColorEditFlags_NoInputs);
            if (aurora::chip(tr("添加箭头"), true, tr("起点 → 终点"))) {
                Workbench::SceneArrow a;
                if (arrowAnchor_ == 0) {
                    a.from = wb.moleculeCenter(static_cast<size_t>(std::max(0, arrowFrom_)));
                    a.to = wb.moleculeCenter(static_cast<size_t>(std::max(0, arrowTo_)));
                } else {
                    a.from = glm::vec3(arrowFromPos_[0], arrowFromPos_[1], arrowFromPos_[2]);
                    a.to = glm::vec3(arrowToPos_[0], arrowToPos_[1], arrowToPos_[2]);
                }
                a.color = arrowColor_;
                a.width = arrowWidth_;
                a.headScale = arrowHead_;
                a.doubleHead = arrowDouble_;
                a.style = arrowStyle_;
                wb.arrows().push_back(a);
            }
            ImGui::Spacing();
            auto& arrows = wb.arrows();
            if (arrows.empty()) {
                ImGui::TextDisabled("%s", tr("还没有箭头"));
            }
            int del = -1;
            for (size_t i = 0; i < arrows.size(); ++i) {
                ImGui::PushID(static_cast<int>(i) + 500);
                bool vis = arrows[i].visible;
                if (ImGui::Checkbox("##av", &vis)) {
                    arrows[i].visible = vis;
                }
                ImGui::SameLine();
                ImGui::Text("箭头 %zu", i + 1);
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("删除"))) {
                    del = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            if (del >= 0) {
                arrows.erase(arrows.begin() + del);
            }
        }
        aurora::cardEnd();

        // —— 水印 ——
        if (aurora::cardBegin("##ann_wm", tr("水印"),
                              tr("图角落的大号半透明文字（示意图底纹，如 TCBG）"))) {
            bool on = wb.watermark().visible;
            if (aurora::toggleRow(tr("显示水印"), &on, tr("随 PNG 导出一起画出"))) {
                wb.watermark().visible = on;
            }
            char wmBuf[128] = {};
            std::snprintf(wmBuf, sizeof(wmBuf), "%s", wb.watermark().text.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##wm_text", tr("水印文字（空 = 不画）"), wmBuf,
                                         sizeof(wmBuf))) {
                wb.watermark().text = wmBuf;
            }
            ImGui::TextColored(aurora::palette().textMuted, "%s", tr("颜色 / 透明度"));
            ImGui::SameLine();
            ImGui::ColorEdit4("##wm_color", &wb.watermark().color.x,
                              ImGuiColorEditFlags_NoInputs);
            float pct = wb.watermark().scale * 100.0f;
            if (aurora::sliderRow(tr("大小（图高 %）"), &pct, 3.0f, 30.0f, "%.0f")) {
                wb.watermark().scale = pct / 100.0f;
            }
            const std::string anchors = trItems({"右下", "左下", "居中", "右上"});
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::Combo("##wm_anchor", &wb.watermark().anchor, anchors.c_str());
        }
        aurora::cardEnd();

        // —— 风格与清理 ——
        if (aurora::cardBegin("##ann_pub", tr("风格与清理"),
                              tr("示意图 / 出版风格 + 一键清除"))) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().textMuted);
            ImGui::TextWrapped("%s", tr("「示意图」= 黑底 + 强轮廓 + 平光 + 无阴影（近 TCBG "
                                        "光合体系图）；「出版」= 黑底 + 平光。"));
            ImGui::PopStyleColor();
            if (aurora::chip(tr("应用示意图风格"), true,
                             tr("黑底、强轮廓、平光、关阴影，并打开水盒与水印"))) {
                wb.applyDiagramStyle();
            }
            ImGui::SameLine();
            if (aurora::chip(tr("应用出版风格"), false)) {
                wb.applyPublicationStyle();
            }
            if (aurora::chip(tr("清除全部标注"), false)) {
                wb.clearAnnotations();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().textFaint);
            ImGui::TextWrapped("%s", tr("文字/箭头/水盒/水印已随 PNG 导出（8-bit 路径）；"
                                        "16-bit 与光追导出暂不含标注。"));
            ImGui::PopStyleColor();
        }
        aurora::cardEnd();
    }

private:
    char  text_[96] = "";
    int   molIndex_ = 0;
    int   bulkCount_ = 8;
    int   labelAnchor_ = 0;                  // 0 分子序号 / 1 坐标 / 2 视图中心
    float labelPos_[3] = {0.0f, 0.0f, 0.0f};
    ImVec4 labelColor_{1.0f, 0.92f, 0.55f, 1.0f};
    float labelSize_ = 18.0f;

    int   arrowAnchor_ = 0;                  // 0 分子序号 / 1 坐标
    int   arrowFrom_ = 0;
    int   arrowTo_ = 1;
    float arrowFromPos_[3] = {0.0f, 0.0f, 0.0f};
    float arrowToPos_[3] = {0.0f, 0.0f, 20.0f};
    int   arrowStyle_ = 0;                   // 0 普通 / 1 波浪（光）
    float arrowWidth_ = 2.2f;
    float arrowHead_ = 1.0f;
    bool  arrowDouble_ = false;
    ImVec4 arrowColor_{1.0f, 0.85f, 0.30f, 1.0f};
};

} // namespace

// —— 工厂（声明见 workbench.h）——
std::unique_ptr<IInspectorModule> makeVisualizeModule() {
    return std::make_unique<VisualizeModule>();
}
std::unique_ptr<IInspectorModule> makeSceneModule() {
    return std::make_unique<SceneModule>();
}
std::unique_ptr<IInspectorModule> makeEspModule() {
    return std::make_unique<EspModule>();
}
std::unique_ptr<IInspectorModule> makeOrbitalModule() {
    return std::make_unique<OrbitalModule>();
}
std::unique_ptr<IInspectorModule> makeTrajectoryModule() {
    return std::make_unique<TrajectoryModule>();
}
std::unique_ptr<IInspectorModule> makeAnalysisModule() {
    return std::make_unique<AnalysisModule>();
}
std::unique_ptr<IInspectorModule> makeLogModule() {
    return std::make_unique<LogModule>();
}
std::unique_ptr<IInspectorModule> makeSettingsModule() {
    return std::make_unique<SettingsModule>();
}
std::unique_ptr<IInspectorModule> makeAnnotationModule() {
    return std::make_unique<AnnotationModule>();
}

} // namespace moleculens::app
