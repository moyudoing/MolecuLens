// =============================================================================
// Moleculens · app · view/tools/reps/reps_tool.cpp —— 图形表示面板实现
// 规格来源：需求附图（VMD Graphical Representations 的四页签与控件）
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/tools/reps/reps_tool.h"

#include <algorithm>
#include <cctype>  // std::tolower（cube 扩展名判断）
#include <cmath>   // std::fabs（Surface Quality 档位匹配）
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "app/controller/io/io_controller.h"
#include "app/controller/rep/representation_controller.h"
#include "app/controller/render/render_controller.h"
#include "app/controller/timeline/timeline_controller.h"
#include "app/data/data_manager.h"
#include "app/view/control/view_control.h"   // openFileDialogEx（ESP cube 文件选择）
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"
#include "engine/render/render_parameter.h"
#include "engine/style/espsurface/esp_surface.h"   // EspColormap 标签 / 默认 iso
#include "entity/scene/scene.h"
#include "entity/selection/parse_table.h"
#include "entity/selection/selection.h"

namespace moleculens::app {

namespace {

// —— Drawing Method（参考图的 27 项 VMD 绘图方法）——
// implemented=false 的项**置灰列出**而不是隐藏：参考图把它们都摆出来了，用户会去
// 找；如实标注"本引擎未实现"远好于静默映射到另一个样式（那会让用户以为选对了）。
struct DrawMethodEntry {
    const char* vmdName;              // 参考图里的名字
    engine::RepresentationType type;  // 对应本引擎的样式（未实现项仅作占位）
    bool implemented;
    const char* note;                 // 未实现时的说明
};

const DrawMethodEntry kDrawMethods[] = {
    {"Lines",        engine::RepresentationType::Wireframe,  true,  nullptr},
    {"Bonds",        engine::RepresentationType::Stick,      true,  nullptr},
    {"DynamicBonds", engine::RepresentationType::Stick,      false, tr("需要按帧重算键表，本引擎键表在导入时固定")},
    {"HBonds",       engine::RepresentationType::Stick,      false, tr("需要氢键判定")},
    {"Points",       engine::RepresentationType::Vdw,        false, tr("本引擎的“点”是体系渲染模式，不是绘图方法")},
    {"VDW",          engine::RepresentationType::Vdw,        true,  nullptr},
    {"CPK",          engine::RepresentationType::Spacefill,  true,  nullptr},
    {"Licorice",     engine::RepresentationType::Licorice,   true,  nullptr},
    {"Polyhedra",    engine::RepresentationType::Stick,      false, tr("未实现")},
    {"Trace",        engine::RepresentationType::Tube,       false, tr("未实现（可用 Tube 近似）")},
    {"Tube",         engine::RepresentationType::Tube,       true,  nullptr},
    {"Ribbons",      engine::RepresentationType::Ribbon,     true,  nullptr},
    {"NewRibbons",   engine::RepresentationType::Ribbon,     false, tr("本引擎只实现了一种丝带（Ribbons）")},
    {"Cartoon",      engine::RepresentationType::Cartoon,    true,  nullptr},
    {"NewCartoon",   engine::RepresentationType::NewCartoon, true,  nullptr},
    {"PaperChain",   engine::RepresentationType::Cartoon,    false, tr("未实现")},
    {"Twister",      engine::RepresentationType::Tube,       false, tr("未实现")},
    {"QuickSurf",    engine::RepresentationType::QuickSurf,  true,  nullptr},
    {"ESP Surface",  engine::RepresentationType::EspSurface, true,  nullptr},
    {"MSMS",         engine::RepresentationType::Surface,    false, tr("未实现（可用 Surf）")},
    {"Surf",         engine::RepresentationType::Surface,    true,  nullptr},
    {"VolumeSlice",  engine::RepresentationType::Surface,    false, tr("需要体数据")},
    {"Isosurface",   engine::RepresentationType::Surface,    false, tr("需要体数据")},
    {"FieldLines",   engine::RepresentationType::Wireframe,  false, tr("需要场数据")},
    {"Orbital",      engine::RepresentationType::Surface,    false, tr("需要量子化学数据")},
    {"Beads",        engine::RepresentationType::Spacefill,  false, tr("未实现")},
    {"Dotted",       engine::RepresentationType::Vdw,        false, tr("未实现")},
    {"Solvent",      engine::RepresentationType::Licorice,   false, tr("未实现（可用选择表达式筛溶剂）")},
};
constexpr int kDrawMethodCount =
    static_cast<int>(sizeof(kDrawMethods) / sizeof(kDrawMethods[0]));

// 当前表示类型 -> Drawing Method 下标（取第一个已实现的匹配项）
int drawMethodIndexFor(engine::RepresentationType type) {
    for (int i = 0; i < kDrawMethodCount; ++i) {
        if (kDrawMethods[i].implemented && kDrawMethods[i].type == type) {
            return i;
        }
    }
    return 1;   // Bonds
}

// 与模型同步的文本缓冲（仅"来源变化"时覆盖，否则会清掉用户正在输入的内容）
bool syncText(char* buf, size_t bufSize, int32_t* bufRep, std::string* bufSource,
              int32_t repId, const std::string& source) {
    if (*bufRep == repId && *bufSource == source) {
        return false;
    }
    std::snprintf(buf, bufSize, "%s", source.c_str());
    *bufRep = repId;
    *bufSource = source;
    return true;
}

// 是否是 cube 文件（.cube/.cub）。ESP 参数面板用它决定"密度 cube 默认 = 当前
// 加载文件"是否成立：打开 density.cube 时场景里的原子就来自它；打开 PDB 时
// 不能把 PDB 当密度体数据塞进去。
bool isCubeFile(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == "cube" || ext == "cub";
}

// 带 |< << >> >| 步进按钮的数值框（参考图里 Bond Radius / Bond Resolution 的样式）。
// 返回 true = 本帧数值被改动。
bool stepperRow(const char* label, float* value, float step, float minV, float maxV,
                bool enabled = true) {
    bool changed = false;
    ImGui::TextUnformatted(label);
    ImGui::SameLine(170.0f);
    ImGui::PushID(label);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton("|<")) { *value = minV; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) { *value = std::max(minV, *value - step); changed = true; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    changed |= ImGui::InputFloat("##v", value, 0.0f, 0.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::SmallButton(">>")) { *value = std::min(maxV, *value + step); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton(">|")) { *value = maxV; changed = true; }
    *value = std::clamp(*value, minV, maxV);
    if (!enabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return changed;
}

// —— Surface Quality 档位（参考图 Draw style 页签的下拉）——
// 档位只是**写同一个底层参数**（网格间距）的快捷方式，与材质预设同一模式：
// 不匹配任何档位时下拉回显"自定义"，用户一眼能看出两者是同一个东西，
// 不会误以为是两个独立参数。
struct QualityPreset {
    const char* name;
    float gridSpacing;
};
const QualityPreset kQualityPresets[] = {
    {"Low", 1.00f}, {"Medium", 0.50f}, {"High", 0.25f}, {"Ultra", 0.15f}};
constexpr int kQualityPresetCount =
    static_cast<int>(sizeof(kQualityPresets) / sizeof(kQualityPresets[0]));

// 当前网格间距对应的档位下标；不匹配任何档位 -> -1（自定义）
int qualityIndexFor(float gridSpacing) {
    for (int i = 0; i < kQualityPresetCount; ++i) {
        if (std::fabs(kQualityPresets[i].gridSpacing - gridSpacing) < 0.005f) {
            return i;
        }
    }
    return -1;
}

// 把材质预设写入表示的 7 个参数字段（材质库是唯一事实来源）
void applyPresetTo(engine::Representation& rep, int presetIndex) {
    if (presetIndex < 0 || presetIndex >= engine::kMaterialPresetCount) {
        return;
    }
    const engine::MaterialPreset& p = engine::materialPresetAt(presetIndex);
    for (int c = 0; c < 3; ++c) {
        rep.ambient[c] = p.ambient[c];
        rep.diffuse[c] = p.diffuse[c];
        rep.specular[c] = p.specular[c];
    }
    rep.ambient[3] = rep.diffuse[3] = rep.specular[3] = 1.0f;
    rep.shininess = p.shininess;
    rep.metallic = p.metallic;
    rep.roughness = p.roughness;
    rep.opacity = p.opacity;
    rep.materialPreset = presetIndex;
}

// —— ESP 参考图默认观感（仅"首次选中且材质仍是出厂默认"时套用一次）——
// 材质仍是出厂默认 = 选中 Opaque 预设且各参数未被手改（Representation 的初值）。
// 用它做门槛：用户已调过材质（materialPreset == -1 或别的预设）时不覆盖。
bool isFactoryDefaultMaterial(const engine::Representation& rep) {
    return rep.materialPreset == 0 && std::fabs(rep.metallic) < 1e-4f &&
           std::fabs(rep.opacity - 1.0f) < 1e-4f &&
           std::fabs(rep.shininess - 32.0f) < 0.01f &&
           std::fabs(rep.roughness - 0.40f) < 0.002f &&
           std::fabs(rep.specular[0] - 0.30f) < 0.002f &&
           std::fabs(rep.ambient[0] - 0.60f) < 0.002f &&
           std::fabs(rep.diffuse[0] - 0.90f) < 0.002f;
}

// 参考图观感：半透明表面（能看见内部球棍）+ 更集中的高光 + 参考配色
// （Reference：低值黄/负电势 -> 紫/正电势，采样自参考截图色标条）。
// 数值按任务规格；用户随后可在面板里任意修改。
void applyEspReferenceLook(engine::Representation& rep) {
    rep.opacity = 0.85f;
    rep.metallic = 0.0f;
    rep.roughness = 0.30f;
    rep.shininess = 48.0f;
    for (int c = 0; c < 3; ++c) {
        rep.ambient[c] = 0.30f;
        rep.diffuse[c] = 0.90f;
        rep.specular[c] = 0.15f;
    }
    rep.ambient[3] = rep.diffuse[3] = rep.specular[3] = 1.0f;
    // 材质数值偏离任一预设 -> 「自定义」（材质下拉如实回显，符合既有约定）
    rep.materialPreset = -1;
    rep.espColormap = static_cast<int32_t>(engine::EspColormap::Reference);
    rep.espInvert = false;
}

} // namespace

void RepsTool::appendToSelection(int32_t repId, const char* token) {
    const auto& reps = repCtl_->representations();
    if (repId < 0 || static_cast<size_t>(repId) >= reps.size() || token == nullptr) {
        return;
    }
    std::string expr = reps[static_cast<size_t>(repId)].selection;
    if (!expr.empty() && expr.back() != ' ') {
        expr += ' ';
    }
    expr += token;
    repCtl_->setSelection(repId, expr);
}

// —— Selected Molecule + 作用域 ——
// 本引擎的表示是**全局**的（渲染器对每条可见表示遍历全部实例），没有"这条表示只
// 属于某个分子"的字段。故参考图里"选分子 -> Create Rep"在这里落成：新建表示时带一个
// 把作用域限定到该分子的 selection（该实例的残基区间）。既保留参考图的交互，
// 又不必改数据模型。
void RepsTool::renderMoleculeBar(DataManager* dm, MolScene* scene) {
    ImGui::TextUnformatted("Selected Molecule");
    const size_t n = dm ? dm->moleculeCount() : 0;
    std::string preview = tr("全部 (all atoms)");
    if (moleculeIdx_ > 0 && static_cast<size_t>(moleculeIdx_ - 1) < n) {
        preview = dm->moleculeName(static_cast<size_t>(moleculeIdx_ - 1));
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##molecule", preview.c_str())) {
        if (ImGui::Selectable(tr("全部 (all atoms)"), moleculeIdx_ == 0)) {
            moleculeIdx_ = 0;
        }
        for (size_t i = 0; i < n; ++i) {
            char label[192];
            std::snprintf(label, sizeof(label), "%s  (%zu 原子)",
                          dm->moleculeName(i).c_str(), dm->atomCount(i));
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label, moleculeIdx_ == static_cast<int>(i) + 1)) {
                moleculeIdx_ = static_cast<int>(i) + 1;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // 由选中分子推导作用域表达式（残基区间）
    scopedSel_.clear();
    const entity::SceneStructure* sc = scene ? scene->scene() : nullptr;
    if (moleculeIdx_ > 0 && sc != nullptr &&
        static_cast<size_t>(moleculeIdx_ - 1) < sc->instances().size()) {
        const auto& inst = sc->instances()[static_cast<size_t>(moleculeIdx_ - 1)];
        const auto& st = sc->structures()[inst.structureIndex];
        int32_t lo = std::numeric_limits<int32_t>::max();
        int32_t hi = std::numeric_limits<int32_t>::min();
        for (const auto& a : st.atoms) {
            lo = std::min(lo, a.residueId);
            hi = std::max(hi, a.residueId);
        }
        if (lo <= hi) {
            char buf[64];
            if (lo == hi) {
                std::snprintf(buf, sizeof(buf), "resid %d", lo);
            } else {
                std::snprintf(buf, sizeof(buf), "resid %d-%d", lo, hi);
            }
            scopedSel_ = buf;
        }
    }
    if (!scopedSel_.empty()) {
        ImGui::TextDisabled(tr("作用域表达式：%s"), scopedSel_.c_str());
    }
    UITheme::helpMarker(tr("选择作用对象。本引擎的表示是全局的（对所有体系生效），\n" "因此这里选择分子只影响 Create Rep 时生成的原子选择表达式；\n" "要限制某条表示作用于部分原子，请用 Selected Atoms / Selections。"));
}

void RepsTool::renderRepTable() {
    const auto& reps = repCtl_->representations();
    ImGui::BeginChild("##reptable", ImVec2(0, 96), true);
    if (ImGui::BeginTable("reps", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Style");
        ImGui::TableSetupColumn("Color");
        ImGui::TableSetupColumn("Selection");
        ImGui::TableHeadersRow();
        for (int32_t i = 0; i < static_cast<int32_t>(reps.size()); ++i) {
            const auto& r = reps[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            // 整行可点（SpanAllColumns）：选中行 = 正在编辑的表示
            char style[96];
            std::snprintf(style, sizeof(style), "%s%s", r.name.c_str(),
                          r.visible ? "" : tr("（隐藏）"));
            if (ImGui::Selectable(style, repCtl_->activeIndex() == i,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                repCtl_->setActiveIndex(i);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(engine::colorSchemeZh(r.colorScheme));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.selection.empty() ? "all" : r.selection.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void RepsTool::renderDrawStyleTab(int32_t repId) {
    const auto& reps = repCtl_->representations();
    if (repId < 0 || static_cast<size_t>(repId) >= reps.size()) {
        return;
    }
    const engine::Representation& model = reps[static_cast<size_t>(repId)];

    // 编辑对象：自动应用时是模型的副本（改完立即提交）；关闭时是暂存副本
    if (!autoApply_ && (stagedRep_ != repId)) {
        staged_ = model;
        stagedRep_ = repId;
        stagedDirty_ = false;
    }
    engine::Representation edit = autoApply_ ? model : staged_;

    // 提交：把 edit 的全部参数写回控制器（表示的唯一持有者）。
    // 逐字段调 setter 会触发多次重建；这里一次性写回由 setMaterial + 其余 setter
    // 组成，正是既有的控制器 API 契约。
    auto commit = [&](const engine::Representation& e) {
        repCtl_->setType(repId, e.type);
        repCtl_->setColorScheme(repId, e.colorScheme);
        repCtl_->setRadiusScale(repId, e.radiusScale);
        repCtl_->setBondScale(repId, e.bondScale);
        repCtl_->setSurfIsovalue(repId, e.surfIsovalue);
        repCtl_->setSurfGridSpacing(repId, e.surfGridSpacing);
        repCtl_->setSystemSurface(repId, e.systemSurface);
        // ESP 映射表面参数（type != EspSurface 时写入也无副作用：字段随表示保存）
        repCtl_->setEspDensityCubePath(repId, e.espDensityCubePath);
        repCtl_->setEspPotentialCubePath(repId, e.espPotentialCubePath);
        repCtl_->setEspIsovalue(repId, e.espIsovalue);
        repCtl_->setEspRange(repId, e.espVminKcal, e.espVmaxKcal);
        repCtl_->setEspAutoRange(repId, e.espAutoRange);
        repCtl_->setEspInvert(repId, e.espInvert);
        repCtl_->setEspColormap(repId, e.espColormap);
        repCtl_->setMaterial(repId, e.materialPreset, e.ambient, e.diffuse, e.specular,
                             e.shininess, e.metallic, e.roughness, e.opacity);
    };
    // 变更后统一走这里：自动应用 -> 立即提交；否则只标记暂存有改动
    auto onChange = [&]() {
        if (autoApply_) {
            commit(edit);
        } else {
            staged_ = edit;
            stagedDirty_ = true;
        }
    };

    // —— Coloring Method ——
    // 按枚举驱动，避免以后新增 ColorScheme 时这里漏项（历史上固定 5 项 → colorIdx=5 越界读）
    const char* colorNames[static_cast<int>(engine::ColorScheme::Count)];
    for (int i = 0; i < static_cast<int>(engine::ColorScheme::Count); ++i) {
        colorNames[i] = engine::colorSchemeZh(static_cast<engine::ColorScheme>(i));
    }
    int colorIdx = static_cast<int>(edit.colorScheme);
    ImGui::TextUnformatted("Coloring Method");
    ImGui::SameLine(170.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##color", &colorIdx, colorNames, IM_ARRAYSIZE(colorNames))) {
        edit.colorScheme = static_cast<engine::ColorScheme>(colorIdx);
        onChange();
    }

    // —— Material（23 套，与 VMD 的 Material 下拉同名）——
    int matIdx = edit.materialPreset;
    if (matIdx < 0 || matIdx > engine::kMaterialPresetCount) {
        matIdx = engine::kMaterialPresetCount;   // 自定义
    }
    const char* matNames[engine::kMaterialPresetCount + 1];
    for (int i = 0; i < engine::kMaterialPresetCount; ++i) {
        matNames[i] = engine::materialPresetAt(i).name;
    }
    matNames[engine::kMaterialPresetCount] = tr("自定义");
    ImGui::TextUnformatted("Material");
    ImGui::SameLine(170.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##material", &matIdx, matNames, engine::kMaterialPresetCount + 1)) {
        if (matIdx < engine::kMaterialPresetCount) {
            applyPresetTo(edit, matIdx);
            onChange();
        }
    }

    // —— Drawing Method（27 项；未实现的置灰）——
    int dmIdx = drawMethodIndexFor(edit.type);
    ImGui::TextUnformatted("Drawing Method");
    ImGui::SameLine(170.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##drawmethod", kDrawMethods[dmIdx].vmdName)) {
        for (int i = 0; i < kDrawMethodCount; ++i) {
            const DrawMethodEntry& e = kDrawMethods[i];
            ImGui::PushID(i);
            if (!e.implemented) {
                ImGui::BeginDisabled();
                ImGui::Selectable(e.vmdName, false);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(tr("本引擎未实现：%s"), e.note ? e.note : "—");
                }
            } else if (ImGui::Selectable(e.vmdName, i == dmIdx)) {
                const engine::RepresentationType oldType = edit.type;
                edit.type = e.type;
                // 线框默认用 VMD 的「按原子名」配色（碳青/氧红/氮蓝/氢白），
                // 否则默认 ElementCPK 下碳灰、氢白，与 VMD 对照图观感差很远。
                // 仅当配色仍是出厂默认时套用；用户改过就不覆盖。
                if (oldType != engine::RepresentationType::Wireframe &&
                    e.type == engine::RepresentationType::Wireframe &&
                    edit.colorScheme == engine::ColorScheme::ElementCPK) {
                    edit.colorScheme = engine::ColorScheme::AtomName;
                }
                // 首次切到 ESP 映射表面、且材质仍是出厂默认 -> 一次性套用参考图观感
                // （半透明 + Viridis 反向 + 更集中的高光）。用户调过材质/再次切回时
                // 不覆盖，避免"改好的参数被吃掉"。
                if (oldType != engine::RepresentationType::EspSurface &&
                    e.type == engine::RepresentationType::EspSurface &&
                    isFactoryDefaultMaterial(edit)) {
                    applyEspReferenceLook(edit);
                }
                onChange();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    UITheme::helpMarker(tr("带灰条的项本引擎未实现（如实标注，不做静默近似映射）。"));

    // —— Default：复位该表示的参数（保留样式与选择表达式）——
    if (ImGui::Button("Default")) {
        applyPresetTo(edit, 0);   // Opaque
        edit.radiusScale = 1.0f;
        edit.bondScale = 1.0f;
        edit.surfIsovalue = 0.5f;
        edit.surfGridSpacing = 0.5f;
        edit.systemSurface = false;
        // ESP 参数复位（cube 路径是数据源，不复位）。材质/配色复位到参考图默认观感
        // （半透明 + Viridis 反向），与"首次选中 ESP"时的默认一致。
        if (edit.type == engine::RepresentationType::EspSurface) {
            edit.espIsovalue = engine::kEspDefaultIsolevel;
            edit.espVminKcal = -45.0f;
            edit.espVmaxKcal = 45.0f;
            edit.espAutoRange = true;
            applyEspReferenceLook(edit);
        }
        onChange();
    }
    ImGui::SameLine();
    UITheme::helpMarker(tr("复位该表示的材质与缩放参数（保留样式与选择表达式）。"));

    ImGui::Separator();
    // 球体缩放（radiusScale）：作用于球类样式（球棍/CPK/空间填充）的原子半径；
    // VDW 样式按定义**不受**该缩放（见 style 注释）。
    // 标签沿用参考图的 "Radius Scale"（作用于球类样式的原子半径；表面样式下
    // 它同时决定原子半径 -> 影响表面外扩范围）
    if (stepperRow(tr("Radius Scale"), &edit.radiusScale, 0.05f, 0.05f, 5.0f)) {
        onChange();
    }
    UITheme::helpMarker(tr("原子球半径的整体缩放（球棍/CPK/空间填充生效；\n" "VDW 样式按定义使用全范德华半径，不受此项影响）。"));
    if (stepperRow("Bond Radius", &edit.bondScale, 0.05f, 0.05f, 5.0f)) {
        onChange();
    }
    UITheme::helpMarker(tr("键半径缩放（真实生效：几何生成时按它算圆柱半径）。"));
    // Bond Resolution：本引擎的键圆柱是**全局共享网格**（固定 8 段），没有逐表示的
    // 细分参数 -> 置灰。放一个改了没反应的控件比不放更糟。
    float bondRes = 12.0f;
    (void)stepperRow("Bond Resolution", &bondRes, 1.0f, 4.0f, 64.0f,
                     /*enabled=*/false);
    UITheme::helpMarker(tr("未实现：键圆柱是全局共享网格（固定 8 段），无逐表示细分参数。"));

    // —— Apply Changes Automatically + Apply / Revert ——
    ImGui::Separator();
    ImGui::Checkbox("Apply Changes Automatically", &autoApply_);
    UITheme::helpMarker(tr("关闭后改动先暂存，点 Apply 才生效（参考图同款行为）。"));
    ImGui::BeginDisabled(autoApply_);
    if (ImGui::Button("Apply")) {
        commit(staged_);
        stagedDirty_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        staged_ = model;
        stagedDirty_ = false;
    }
    ImGui::EndDisabled();
    if (!autoApply_ && stagedDirty_) {
        ImGui::SameLine();
        ImGui::TextColored(ui::C_WARN, tr("有未应用的改动"));
    }

    // —— 材质逐项参数（透明度/光泽度等；材质面板另有完整版）——
    UITheme::sectionHeader(tr("材质参数"));
    bool matChanged = false;
    matChanged |= ImGui::SliderFloat(tr("透明度 (opacity)"), &edit.opacity, 0.05f, 1.0f);
    matChanged |= ImGui::SliderFloat(tr("光泽度 (gloss)"), &edit.shininess, 1.0f, 256.0f,
                                     "%.0f");
    matChanged |= ImGui::SliderFloat(tr("粗糙度"), &edit.roughness, 0.0f, 1.0f);
    matChanged |= ImGui::SliderFloat(tr("金属度"), &edit.metallic, 0.0f, 1.0f);
    if (matChanged) {
        edit.materialPreset = -1;   // 手改任一参数 -> 自定义
        onChange();
    }

    // —— 表面参数（参考图 Draw style 页签下半部分的控件组）——
    // QuickSurf / Surface 共用本引擎的**同一个**网格间距参数；参考图里两个方法
    // 的叫法不同（QuickSurf 叫 Resolution，Surf 叫 Grid Spacing），这里按方法切换
    // 标签以对齐参考图，但底层仍是同一个值 —— 不摆两个控件去改同一个参数。
    const bool isQuick = edit.type == engine::RepresentationType::QuickSurf;
    const bool isSurface =
        isQuick || edit.type == engine::RepresentationType::Surface;
    if (isSurface) {
        UITheme::sectionHeader(tr("表面参数"));
        bool surfChanged = false;

        // Resolution（QuickSurf）/ Grid Spacing（Surface）：都是 surfGridSpacing，
        // 越小越精细、越慢
        surfChanged |= stepperRow(isQuick ? tr("Resolution") : tr("Grid Spacing"),
                                  &edit.surfGridSpacing, 0.05f, 0.1f, 2.0f);
        if (isQuick) {
            surfChanged |= stepperRow(tr("Density Isovalue"), &edit.surfIsovalue,
                                      0.05f, 0.05f, 2.0f);
        }
        // Surface Quality：网格间距的档位预设（不匹配则回显"自定义"）
        int qIdx = qualityIndexFor(edit.surfGridSpacing);
        const char* qNames[kQualityPresetCount + 1];
        for (int i = 0; i < kQualityPresetCount; ++i) {
            qNames[i] = kQualityPresets[i].name;
        }
        qNames[kQualityPresetCount] = tr("自定义");
        int qCombo = qIdx >= 0 ? qIdx : kQualityPresetCount;
        ImGui::TextUnformatted(tr("Surface Quality"));
        ImGui::SameLine(170.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##quality", &qCombo, qNames, kQualityPresetCount + 1)) {
            if (qCombo < kQualityPresetCount) {
                edit.surfGridSpacing = kQualityPresets[qCombo].gridSpacing;
                surfChanged = true;
            }
        }
        if (surfChanged) {
            onChange();
        }
        UITheme::helpMarker(
            tr("Resolution / Grid Spacing 是同一个参数（本引擎两种表面共用一套网格）：\n"
               "越小越精细、计算越慢。Surface Quality 是它的档位快捷方式，\n"
               "不等于任何档位时显示「自定义」。"));
        ImGui::Spacing();
        if (ImGui::Checkbox(tr("整体系合并表面"), &edit.systemSurface)) {
            onChange();
        }
        // 用 ASCII "->" 而非 "->"：OPPO_B.ttf 的 cmap 里没有 U+2192，
        // 补进 glyph ranges 也拿不到字形，只会渲染成方块（见 tools/check_font_glyphs.py）
        UITheme::helpMarker(tr("整个体系生成一个表面，而非每个分子一个。\n" "溶剂化体系差异极大（实测 md.gro：QuickSurf 846 -> 170 ms，\n" "命令流 316 MB -> 19.8 MB）。"));
    }

    // —— ESP 映射表面参数（电子密度等值面 + 静电势逐顶点着色；输入 cube 文件）——
    if (edit.type == engine::RepresentationType::EspSurface) {
        if (renderEspParams(edit)) {
            onChange();
        }
    }
}

// ESP 映射表面参数（Draw style 页签）：输入 cube 文件 + 等值面 + 色标。
// 控件直接绑定 edit（自动应用时它是模型副本，改完由调用方 commit；否则进暂存）。
bool RepsTool::renderEspParams(engine::Representation& edit) {
    UITheme::sectionHeader(tr("ESP 参数"));
    bool changed = false;

    // —— 密度 cube：默认 = 当前加载文件（仅当打开的就是 cube）——
    // 打开 density.cube 时场景里的原子来自该文件，它天然就是密度 cube；打开
    // PDB/GRO 时留空，让用户用「浏览...」显式选择（不能把结构文件当体数据）。
    if (edit.espDensityCubePath.empty() && ioCtl_ != nullptr &&
        isCubeFile(ioCtl_->currentSourcePath())) {
        edit.espDensityCubePath = ioCtl_->currentSourcePath();
        changed = true;
    }

    const std::string cubeFilter = trItems({"Gaussian cube 文件 (*.cube;*.cub)",
                                            "*.cube;*.cub", "所有文件 (*.*)", "*.*"});

    // 一行：标签 + 只读路径（可选中复制）+ 浏览按钮。路径为空时显示提示串。
    const auto pathRow = [&](const char* rowId, const char* label, std::string& path,
                             const char* emptyHint) {
        ImGui::PushID(rowId);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(170.0f);
        ImGui::SetNextItemWidth(-84.0f);
        // ReadOnly InputText：不可编辑但可选中/复制（TextWrapped 做不到）
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", path.empty() ? emptyHint : path.c_str());
        ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button(tr("浏览..."))) {
            const std::string chosen =
                ViewControl::openFileDialogEx(cubeFilter.c_str(), "cube");
            if (!chosen.empty()) {
                path = chosen;
                changed = true;
            }
        }
        ImGui::PopID();
    };

    pathRow("esp_density", tr("密度 cube（几何）"), edit.espDensityCubePath,
            ioCtl_ != nullptr && isCubeFile(ioCtl_->currentSourcePath())
                ? tr("（默认：当前加载的 cube 文件）")
                : tr("（点击「浏览...」选择密度 cube 文件）"));
    pathRow("esp_potential", tr("静电势 cube（颜色）"), edit.espPotentialCubePath,
            tr("（必填：选择静电势 cube 文件）"));

    // 缺文件时明确提示：引擎层 buildEspSurface 失败只写日志、不绘制，界面上没有
    // 任何反馈 —— 这里把"为什么看不到表面"提前说出来。
    if (edit.espDensityCubePath.empty() || edit.espPotentialCubePath.empty()) {
        ImGui::TextColored(ui::C_WARN, "%s",
                           edit.espPotentialCubePath.empty()
                               ? tr("未选择静电势 cube：本表示不会绘制表面")
                               : tr("未确定密度 cube：本表示不会绘制表面"));
    }

    // —— 等值面阈值（密度，a.u.；0.001 ≈ vdW 表面）——
    ImGui::PushID("esp_iso");
    ImGui::TextUnformatted(tr("等值面阈值 iso (a.u.)"));
    ImGui::SameLine(170.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##iso", &edit.espIsovalue, 0.0005f, 0.02f, "%.4f",
                           ImGuiSliderFlags_Logarithmic)) {
        changed = true;
    }
    ImGui::PopID();
    // 常用档位快捷按钮（0.001 = 化学惯例的电子密度等值面）
    static const float kIsoPresets[] = {0.0005f, 0.001f, 0.002f, 0.005f, 0.01f};
    ImGui::TextUnformatted(tr("常用值"));
    ImGui::SameLine(170.0f);
    for (int i = 0; i < IM_ARRAYSIZE(kIsoPresets); ++i) {
        ImGui::PushID(i);
        char label[16];
        std::snprintf(label, sizeof(label), "%.4f", kIsoPresets[i]);
        if (ImGui::SmallButton(label)) {
            edit.espIsovalue = kIsoPresets[i];
            changed = true;
        }
        ImGui::PopID();
        if (i + 1 < IM_ARRAYSIZE(kIsoPresets)) {
            ImGui::SameLine();
        }
    }
    UITheme::helpMarker(tr("电子密度等值面阈值（a.u.）：越小表面越外扩（0.001 约等于 vdW 表面，0.0005 更外扩、0.005 更贴近原子核）。"));

    // —— 色标范围（kcal/mol）——
    ImGui::PushID("esp_autorange");
    ImGui::TextUnformatted(tr("自动范围"));
    ImGui::SameLine(170.0f);
    if (ImGui::Checkbox("##auto", &edit.espAutoRange)) {
        changed = true;
    }
    ImGui::PopID();
    UITheme::helpMarker(tr("按 ESP 数据自动定色标范围（发散型取 ±max|v| 的 99.5 分位）；关闭后用下面的固定上下限。"));

    ImGui::BeginDisabled(edit.espAutoRange);
    ImGui::PushID("esp_range");
    ImGui::TextUnformatted(tr("色标范围 (kcal/mol)"));
    ImGui::SameLine(170.0f);
    ImGui::SetNextItemWidth(84.0f);
    changed |= ImGui::InputFloat("##min", &edit.espVminKcal, 1.0f, 10.0f, "%.1f");
    ImGui::SameLine();
    ImGui::TextUnformatted("~");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(84.0f);
    changed |= ImGui::InputFloat("##max", &edit.espVmaxKcal, 1.0f, 10.0f, "%.1f");
    ImGui::PopID();
    ImGui::EndDisabled();
    // 上下限反了时即时纠正（否则 valueToT 的 span 退化 -> 整面单色）
    if (edit.espVmaxKcal <= edit.espVminKcal) {
        edit.espVmaxKcal = edit.espVminKcal + 1.0f;
        changed = true;
    }

    // —— 色标方案（7 项：RWB / BWR / Coolwarm / Turbo / Viridis / Jet / Reference）——
    ImGui::PushID("esp_colormap");
    ImGui::TextUnformatted(tr("色标方案"));
    ImGui::SameLine(170.0f);
    ImGui::SetNextItemWidth(-1.0f);
    const int cmapCount = static_cast<int>(engine::EspColormap::Count);
    int cmap = std::clamp(static_cast<int>(edit.espColormap), 0, cmapCount - 1);
    std::string cmapItems;
    for (int i = 0; i < cmapCount; ++i) {
        // 引擎返回中文显示名，界面层统一走 tr()
        cmapItems += tr(engine::espColormapLabelZh(static_cast<engine::EspColormap>(i)));
        cmapItems.push_back('\0');
    }
    if (ImGui::Combo("##cmap", &cmap, cmapItems.c_str())) {
        edit.espColormap = cmap;
        changed = true;
    }
    ImGui::PopID();

    ImGui::PushID("esp_invert");
    ImGui::TextUnformatted(tr("翻转色标"));
    ImGui::SameLine(170.0f);
    if (ImGui::Checkbox("##invert", &edit.espInvert)) {
        changed = true;
    }
    ImGui::PopID();
    UITheme::helpMarker(tr("翻转配色方向（默认参考配色「黄->绿->青->蓝->紫」：负电势黄、正电势紫；首次选中 ESP 表示时自动套用参考图观感，用户仍可改）。"));

    // —— 极值标注：引擎 Representation 无"显示极值标注"字段 -> 固定开启 ——
    // 置灰勾选而不是隐藏：如实告诉用户这个开关存在、但当前恒开，不会让人以为
    // 关掉过，也不会让用户去找一个不存在的开关。
    bool extremaAlwaysOn = true;
    ImGui::PushID("esp_extrema");
    ImGui::BeginDisabled();
    ImGui::Checkbox(tr("显示极值标注"), &extremaAlwaysOn);
    ImGui::EndDisabled();
    ImGui::PopID();
    UITheme::helpMarker(tr("引擎层无逐表示开关：色标条与极值数值在视口和导出的 PNG 中固定显示（最多 8 个极大 + 8 个极小）。"));

    // 不透明度复用下方「材质参数」的同一个 opacity 字段（不再摆第二个滑杆）
    ImGui::TextDisabled("%s",
                        tr("不透明度：用下方「材质参数」的透明度 (opacity) 滑杆；首次选中 ESP 时会自动设为 0.85（半透明，便于看见内部球棍，同时保持 ESP 颜色饱和度），可随时改。"));

    return changed;
}

void RepsTool::renderSelectionsTab(int32_t repId) {
    const entity::ParseTable& table = entity::ParseTable::defaultTable();

    // —— Singlewords：列表来自解析器的真实注册表（不手抄）——
    ImGui::TextUnformatted("Singlewords");
    ImGui::BeginChild("##singlewords", ImVec2(160, 118), true);
    const std::vector<std::string> words = table.singleWordNames();
    for (int i = 0; i < static_cast<int>(words.size()); ++i) {
        const bool impl = table.isWordImplemented(words[static_cast<size_t>(i)]);
        ImGui::PushID(i);
        if (!impl) {
            // 置灰：占位宏不产生掩码，选中会让表示直接消失（且无报错）
            ImGui::BeginDisabled();
            ImGui::Selectable(words[static_cast<size_t>(i)].c_str(), false);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(tr("占位宏：解析器已注册但未实现，选中会产生空选择"));
            }
        } else if (ImGui::Selectable(words[static_cast<size_t>(i)].c_str(),
                                     singleWordIdx_ == i)) {
            singleWordIdx_ = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button("and")) { appendToSelection(repId, "and"); }
    ImGui::SameLine();
    if (ImGui::Button("or")) { appendToSelection(repId, "or"); }
    ImGui::SameLine();
    if (ImGui::Button("not")) { appendToSelection(repId, "not"); }
    ImGui::Spacing();
    if (ImGui::Button("Apply")) {
        if (singleWordIdx_ >= 0 && singleWordIdx_ < static_cast<int>(words.size())) {
            appendToSelection(repId, words[static_cast<size_t>(singleWordIdx_)].c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        repCtl_->setSelection(repId, std::string());
    }
    ImGui::EndGroup();

    // —— Macro definition（本引擎无宏机制 -> 置灰）——
    ImGui::Separator();
    ImGui::TextUnformatted("Macro definition");
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##macro", macroName_, sizeof(macroName_));
    ImGui::EndDisabled();
    UITheme::helpMarker(tr("宏定义未实现：解析器不支持自定义宏，故置灰。"));

    // —— Keyword / Value：关键字列表来自解析器的真实支持集 ——
    ImGui::Separator();
    size_t kwCount = 0;
    const char* const* kws = entity::selectionKeywords(&kwCount);
    ImGui::BeginChild("##keywords", ImVec2(160, 110), true);
    for (int i = 0; i < static_cast<int>(kwCount); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(kws[i], keywordIdx_ == i)) {
            keywordIdx_ = i;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Value");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("##value", valueBuf_, sizeof(valueBuf_));
    if (ImGui::Button(tr("插入"))) {
        if (keywordIdx_ >= 0 && keywordIdx_ < static_cast<int>(kwCount)) {
            std::string token = std::string(kws[keywordIdx_]) + " " + valueBuf_;
            appendToSelection(repId, token.c_str());
        }
    }
    ImGui::EndGroup();
    UITheme::helpMarker(tr("关键字列表直接来自解析器（selectionKeywords），\n" "故列出的每一项都真的能解析。当前支持：\n" "name / resid / chain / element / occupancy / bfactor；\n" "另有 within <距离> of <条件> 与 same residue as <条件>。"));
}

void RepsTool::renderTrajectoryTab() {
    if (timelineCtl_ == nullptr) {
        ImGui::TextDisabled(tr("时间轴未就绪"));
        return;
    }
    ImGui::TextDisabled(tr("说明：帧范围/步长属于**时间轴**（全局），不是逐表示参数。"));
    ImGui::TextDisabled(tr("本页签直接操作时间轴，与时间轴面板是同一份状态。"));
    ImGui::Separator();
    uint64_t inF = timelineCtl_->inFrame();
    uint64_t outF = timelineCtl_->outFrame();
    ImGui::InputScalar(tr("起始帧"), ImGuiDataType_U64, &inF);
    ImGui::InputScalar(tr("结束帧"), ImGuiDataType_U64, &outF);
    if (ImGui::Button(tr("应用帧范围"))) {
        timelineCtl_->setInOut(inF, outF);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("全范围"))) {
        timelineCtl_->setInOut(0, 0);
    }
    UITheme::helpMarker(tr("结束帧 = 0 表示末帧（与时间轴 In/Out 同义）。"));
    ImGui::Separator();
    ImGui::TextDisabled(tr("Update Style（VMD 的逐帧更新方式）在本引擎是内建的：\n" "帧切走坐标重放（只重写坐标载荷，不重建命令流）。\n" "含表面的表示无法重放（顶点数随坐标变化），会退回整条重建。"));
}

void RepsTool::renderPeriodicTab(MolScene* scene) {
    const entity::SceneStructure* sc = scene ? scene->scene() : nullptr;
    if (sc == nullptr || sc->instances().empty()) {
        ImGui::TextDisabled(tr("未加载场景"));
        return;
    }
    // 盒子是**逐帧**的（Timestep::box）：取首个带帧数据的实例
    bool haveBox = false;
    glm::vec3 lens(0.0f);
    for (const auto& inst : sc->instances()) {
        if (inst.trajectory && !inst.trajectory->frames.empty()) {
            const auto& box = inst.trajectory->frames[0].box;
            lens = glm::vec3(box[0], box[4], box[8]);
            haveBox = lens.x > 0.0f || lens.y > 0.0f || lens.z > 0.0f;
            break;
        }
    }
    if (!haveBox) {
        ImGui::TextDisabled(tr("该体系无周期盒信息（正交盒对角线全 0）"));
    } else {
        ImGui::Text(tr("周期盒（正交）: %.2f × %.2f × %.2f Å"), lens.x, lens.y, lens.z);
    }
    ImGui::Separator();
    ImGui::TextUnformatted(tr("本引擎已实现的周期处理："));
    ImGui::BulletText(tr("跨边界分子补齐（unwrap）—— 否则会被画成一根盒长的长棍"));
    ImGui::BulletText(tr("判键按最小镜像（避免跨边界假键与漏键）"));
    ImGui::Separator();
    ImGui::TextUnformatted(tr("未实现（置灰而非静默忽略）："));
    bool showImages = false;
    ImGui::BeginDisabled();
    ImGui::Checkbox(tr("显示周期镜像"), &showImages);
    ImGui::EndDisabled();
    UITheme::helpMarker(tr("周期镜像（把相邻盒子里的副本一并画出）与盒边框绘制未实现。"));
}

void RepsTool::render(DataManager* dm, MolScene* scene) {
    if (!repCtl_) {
        ImGui::TextDisabled(tr("表示控制器未就绪"));
        return;
    }

    // ① 顶部：分子选择 + 增删
    renderMoleculeBar(dm, scene);
    if (ImGui::Button("Create Rep")) {
        const int32_t id = repCtl_->addRepresentation(
            std::string(), engine::RepresentationType::BallAndStick, scopedSel_,
            engine::ColorScheme::ElementCPK);
        if (id >= 0) {
            repCtl_->setActiveIndex(id);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Rep")) {
        deleteTarget_ = repCtl_->activeIndex();
    }

    // ② 表示表格
    renderRepTable();

    const auto& reps = repCtl_->representations();
    if (reps.empty()) {
        ImGui::TextDisabled(tr("无表示：点 Create Rep 新建"));
        return;
    }
    // 删除放在表格绘制之后（循环内 erase 会让本帧余下的行读到失效引用）
    if (deleteTarget_ >= 0) {
        repCtl_->removeRepresentation(deleteTarget_);
        deleteTarget_ = -1;
        return;
    }
    const int32_t repId = repCtl_->activeIndex();
    const engine::Representation& cur = reps[static_cast<size_t>(repId)];

    // ③ Selected Atoms（选择表达式；回车/失焦才提交 —— 逐键提交会每敲一个字符重建）
    ImGui::TextUnformatted("Selected Atoms");
    if (syncText(atomsBuf_, sizeof(atomsBuf_), &atomsBufRep_, &atomsBufSource_, repId,
                 cur.selection)) {
        atomsBufValid_ = true;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool entered =
        ImGui::InputText("##atoms", atomsBuf_, sizeof(atomsBuf_),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
        entity::Selection probe;
        atomsBufValid_ = atomsBuf_[0] == '\0' ||
                         probe.parse(atomsBuf_, entity::ParseTable::defaultTable());
        if (atomsBufValid_) {
            repCtl_->setSelection(repId, atomsBuf_);
            atomsBufSource_ = atomsBuf_;
        }
    }
    if (!atomsBufValid_) {
        ImGui::TextColored(ui::C_DANGER, tr("表达式无法解析，未应用（仍为：%s）"),
                           cur.selection.empty() ? "all" : cur.selection.c_str());
    }

    // ④ 四个页签
    if (ImGui::BeginTabBar("##rep_tabs")) {
        if (ImGui::BeginTabItem("Draw style")) {
            renderDrawStyleTab(repId);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Selections")) {
            renderSelectionsTab(repId);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Trajectory")) {
            renderTrajectoryTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Periodic")) {
            renderPeriodicTab(scene);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace moleculens::app
