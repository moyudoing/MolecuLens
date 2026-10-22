// =============================================================================
// MolecuLens v2 · app/workbench/modules.cpp —— 9 个检查器模块
//
// 检查器是 v2 的"参数面板"：每个模块只负责**一个关注点**的控件布局，
// 状态一律经控制器改（这里不直接碰 Renderer / SceneStructure 的可变状态）。
// 模块通过 IInspectorModule 注册进 Workbench，左导航栏按注册顺序排列，
// 新增模块无需改动布局代码 —— 这就是 v2 "模块化"在 UI 层的落点。
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
                                   "统一色", "按原子名 (VMD)"};

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
    bool valid = true;   // 是否是本 UI 能识别的 resname 形式（空 / resname ... / not resname ...）
    bool negated = false;
    std::vector<std::string> names;
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
    if (toks.empty()) {
        return out;   // 空表达式 = 全部
    }
    size_t i = 0;
    if (toks[0] == "not" || toks[0] == "NOT") {
        out.negated = true;
        ++i;
    }
    if (i >= toks.size() || (toks[i] != "resname" && toks[i] != "RESNAME")) {
        out.valid = false;   // 复杂表达式：UI 不接管，视为全部
        return out;
    }
    for (++i; i < toks.size(); ++i) {
        out.names.push_back(toks[i]);
    }
    return out;
}

bool selectionIncludesGroup(const std::string& sel, const std::string& name) {
    const ResnameSelection ps = parseResnameSelection(sel);
    if (!ps.valid) {
        return true;
    }
    const bool listed = std::find(ps.names.begin(), ps.names.end(), name) != ps.names.end();
    if (ps.negated) {
        return !listed;
    }
    return ps.names.empty() ? true : listed;
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
        out = "resname";
        for (const auto& n : included) {
            out += ' ';
            out += n;
        }
    } else {
        out = "not resname";
        for (const auto& n : excluded) {
            out += ' ';
            out += n;
        }
    }
    return out;
}

// 一键风格：VMD「Glass + DynamicBonds」观感（v2.6）
//   表面 = 整体系 QuickSurf + Glass2 + 不透明度 0.32 + Name 配色
//   球棍 = Opaque + Name 配色（在玻璃壳内部，透过壳可见）
//   背景 = 近白（与 VMD 参考图一致的棚拍观感）
[[maybe_unused]] void applyVmdGlassPreset(Workbench& wb) {
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
// v2.9：表示表格（VMD Graphical Representations 的对照布局）。
// 一屏列出「名称 / 样式 / 配色 / 选择」，点行切换当前编辑对象，行尾 × 直接删除；
// 比「下拉选一个再编辑」更适合 3 条以上表示的场合。
// 「作用分子」编辑器（v2.9）：顶部三个快捷按钮 + 按结构勾选。
// 表格「选择」列弹层与显示面板的折叠区共用。
static void renderMolScopeEditor(Workbench& wb, int32_t repId, const std::string& curSel) {
    DataManager* dm = wb.data();
    RepresentationController* ctl = wb.reps();
    if (dm == nullptr || ctl == nullptr) {
        ImGui::TextDisabled("%s", tr("数据未就绪"));
        return;
    }
    const std::vector<RepMolGroup> groups = collectRepMolGroups(dm);
    if (groups.empty()) {
        ImGui::TextDisabled("%s", tr("场景为空"));
        return;
    }
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
    aurora::spacer(2.0f);
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const RepMolGroup& g = groups[gi];
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
        char info[96];
        std::snprintf(info, sizeof(info), "×%zu (%zu)", g.instances, g.atoms);
        ImGui::TextDisabled("%s", info);
        ImGui::PopID();
    }
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
        // 选中行的底色由我们自己画：把主题主色向正文墨色压暗，
        // 浅粉/浅蓝在白面板上也能形成清楚的中等饱和底色。
        const auto& pal = aurora::palette();
        const ImVec4 selBg(pal.accent.x * 0.72f + pal.text.x * 0.28f,
                           pal.accent.y * 0.72f + pal.text.y * 0.28f,
                           pal.accent.z * 0.72f + pal.text.z * 0.28f, 1.0f);
        const ImVec4 selEdge(pal.accent.x * 0.45f + pal.text.x * 0.55f,
                             pal.accent.y * 0.45f + pal.text.y * 0.55f,
                             pal.accent.z * 0.45f + pal.text.z * 0.55f, 1.0f);
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
        if (!rep.visible) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().textMuted);
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
            // 4px 深色竖条 + 1px 描边，保证即使底色被主题调淡也能看出当前表示。
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, ImVec2(a.x + 4.0f, b.y), aurora::col(selEdge), 2.0f);
            dl->AddRect(a, b, aurora::col(selEdge), 2.0f, 0, 1.0f);
        }
        if (!rep.visible) {
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
        // v2.9：样式/配色单元格可点击，弹出 VMD 式快速选择（不必先选中行再滚到下方表单）
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
    const char* label() const override { return "渲染视图"; }
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
            const bool ok =
                selBuf_[0] == '\0' ||
                probe.parse(selBuf_, entity::ParseTable::defaultTable());
            if (ok) {
                ctl->setSelection(id, selBuf_);
                selSource_ = selBuf_;
            } else {
                selValid_ = false;
            }
        }
        if (!selValid_) {
            ImGui::PushStyleColor(ImGuiCol_Text, aurora::palette().danger);
            ImGui::TextWrapped("%s", tr("表达式无法解析，未应用"));
            ImGui::PopStyleColor();
            selValid_ = true;
        }
        ImGui::TextDisabled("%s", tr("例：not element H · resid 1-20 · within 4 of element Fe"));

        // 作用分子（v2.9，VMD 式）：直接勾分子，自动改写上面的选择表达式
        if (wb.data() != nullptr &&
            ImGui::TreeNodeEx(tr("作用分子（多选）"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            DataManager* dm = wb.data();
            std::vector<std::string> names;
            for (size_t i = 0; i < dm->moleculeCount(); ++i) {
                const std::string nm = groupBaseName(dm->moleculeName(i));
                if (!nm.empty() && std::find(names.begin(), names.end(), nm) == names.end()) {
                    names.push_back(nm);
                }
            }
            std::vector<bool> on(names.size(), false);
            for (size_t i = 0; i < names.size(); ++i) {
                on[i] = r.selection.find("resname " + names[i]) != std::string::npos;
            }
            bool changed = false;
            for (size_t i = 0; i < names.size(); ++i) {
                bool v = on[i];
                ImGui::PushID(static_cast<int>(i) + 7000);
                if (ImGui::Checkbox(names[i].c_str(), &v)) {
                    changed = true;
                    on[i] = v;
                }
                ImGui::PopID();
                if ((i % 2) == 0 && i + 1 < names.size()) {
                    ImGui::SameLine(0.0f, 18.0f);
                }
            }
            if (changed) {
                std::string expr;
                for (size_t i = 0; i < names.size(); ++i) {
                    if (!on[i]) {
                        continue;
                    }
                    if (!expr.empty()) {
                        expr += " or ";
                    }
                    expr += "resname " + names[i];
                }
                ctl->setSelection(id, expr);
                selSource_ = expr;
                std::snprintf(selBuf_, sizeof(selBuf_), "%s", expr.c_str());
            }
            if (names.empty()) {
                ImGui::TextDisabled("%s", tr("场景里还没有分子"));
            } else {
                ImGui::TextDisabled("%s", tr("勾选的分子会拼成 resname A or resname B 表达式"));
            }
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
        int32_t id = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(reps.size()); ++i) {
            const auto t = reps[static_cast<size_t>(i)].type;
            if (t == RepresentationType::Surface || t == RepresentationType::QuickSurf) {
                id = i;
                break;
            }
        }
        if (id < 0) {
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
        const Representation& r = reps[static_cast<size_t>(id)];
        ImGui::TextColored(aurora::palette().textMuted, "%s",
                           tr("正在编辑：") );
        ImGui::SameLine();
        ImGui::TextUnformatted(repLabel(reps, static_cast<size_t>(id)).c_str());

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

        aurora::sliderNum(tr("播放速度"), &speed_, 0.1f, 10.0f, "%.2f×", 0.1f);
        if (speed_ != lastSpeed_) {
            tl->setSpeed(speed_);
            lastSpeed_ = speed_;
        }
        // 播放帧率与物理 dt 分离：dt=10 ps 的轨迹若按 dt 定时会变成 10 秒一帧。
        // 这里让用户选实时帧率（默认 30），速度倍率再乘上去。
        static const float kPlaybackFps[] = {12.0f, 20.0f, 24.0f, 30.0f, 60.0f};
        static const char* const kPlaybackFpsNames[] = {"12 FPS", "20 FPS", "24 FPS",
                                                         "30 FPS", "60 FPS"};
        int fpsIdx = 3;
        for (int i = 0; i < 5; ++i) {
            if (std::fabs(tl->playbackFps() - kPlaybackFps[i]) < 0.5f) {
                fpsIdx = i;
                break;
            }
        }
        std::string fpsItems;
        for (const char* n : kPlaybackFpsNames) {
            fpsItems += n;
            fpsItems.push_back('\0');
        }
        fpsItems.push_back('\0');
        if (aurora::comboRow(tr("播放帧率"), &fpsIdx, fpsItems.c_str(),
                             tr("实时动画每秒推进多少轨迹帧（与轨迹 dt 无关）"))) {
            tl->setPlaybackFps(kPlaybackFps[std::clamp(fpsIdx, 0, 4)]);
        }
        char rate[96];
        std::snprintf(rate, sizeof(rate), "%s %.0f × %.2f = %.0f FPS", tr("实际推进"),
                      static_cast<double>(tl->playbackFps()),
                      static_cast<double>(tl->speed()),
                      static_cast<double>(tl->playbackFps() * tl->speed()));
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
// 7. 分析（Analysis）—— 场景统计（只读）
// =============================================================================
class AnalysisModule : public IInspectorModule {
public:
    const char* id() const override { return "analysis"; }
    const char* label() const override { return "分析"; }
    const char* subtitle() const override { return "场景统计与结构信息"; }

    void render(Workbench& wb) override {
        DataManager* dm = wb.data();
        if (dm == nullptr) {
            ImGui::TextDisabled("%s", tr("数据未就绪"));
            return;
        }
        size_t atoms = 0;
        size_t bonds = 0;
        size_t frames = 0;
        for (size_t i = 0; i < dm->moleculeCount(); ++i) {
            atoms += dm->atomCount(i);
            bonds += dm->bondCount(i);
            frames = std::max(frames, dm->frameCount(i));
        }
        aurora::sectionLabel(tr("总览"));
        aurora::kvRow(tr("分子数"), std::to_string(dm->moleculeCount()).c_str());
        aurora::kvRow(tr("体系数"), std::to_string(dm->systemCount()).c_str());
        aurora::kvRow(tr("原子数"), std::to_string(atoms).c_str());
        aurora::kvRow(tr("键数"), std::to_string(bonds).c_str());
        aurora::kvRow(tr("帧数"), std::to_string(frames).c_str());
        aurora::kvRow(tr("表示数"),
                      std::to_string(wb.representations().size()).c_str());

        aurora::spacer(6.0f);
        char summary[512];
        std::snprintf(summary, sizeof(summary),
                      "MolecuLens v2 场景统计\n分子 %zu · 体系 %zu · 原子 %zu · 键 %zu · 帧 %zu",
                      dm->moleculeCount(), dm->systemCount(), atoms, bonds, frames);
        if (aurora::chip(tr("复制统计到剪贴板"), false)) {
            ImGui::SetClipboardText(summary);
            wb.notify(tr("统计已复制"));
        }

        aurora::sectionLabel(tr("逐分子"));
        for (size_t i = 0; i < dm->moleculeCount(); ++i) {
            const std::string name = dm->moleculeName(i);
            char row[160];
            std::snprintf(row, sizeof(row), "%s  (%zu 原子 / %zu 键)",
                          name.empty() ? tr("未命名") : name.c_str(), dm->atomCount(i),
                          dm->bondCount(i));
            ImGui::BulletText("%s", row);
        }

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
