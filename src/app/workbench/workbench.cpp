// =============================================================================
// MolecuLens v2 · app/workbench/workbench.cpp —— 工作台布局与全局交互
// =============================================================================

#include "app/workbench/workbench.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "app/controller/camera/camera_controller.h"
#include "app/controller/io/io_controller.h"
#include "app/controller/render/render_controller.h"
#include "app/controller/rep/representation_controller.h"
#include "app/controller/scene/scene_controller.h"
#include "app/controller/timeline/timeline_controller.h"
#include "app/data/data_manager.h"
#include "app/i18n/language.h"
#include "app/view/chrome/window_chrome.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/timeline/time_axis.h"
#include "app/view/viewport/mol_scene.h"
#include "app/workbench/style/aurora.h"
#include "engine/camera/camera.h"
#include "engine/export/overlay/annotation/scene_overlay.h"   // 标注快照（PNG 导出共用）
#include "engine/export/overlay/esp_overlay_raster.h"   // projectToPixel（叠加层投影）
#include "engine/render/renderer.h"
#include "engine/style/espsurface/esp_surface.h"
#include "entity/scene/scene.h"
#include "io/cube/cube_volume.h"
#include "io/registry/parser_registry.h"   // allExtensions（对话框过滤器与解析器同源）
#include "persist/settings/settings.h"

namespace moleculens::app {
namespace {

using aurora::col;
using aurora::palette;
using aurora::Palette;
using persist::Settings;
using persist::SettingsStore;

ImVec2 add(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }

// 在指定矩形里开始一个"区域"子窗口。所有区域都禁用滚动，滚动只发生在
// 检查器内部的内容子窗口。
void beginRegion(const char* id, const ImVec2& pos, const ImVec2& size, const ImVec4& bg,
                 float rounding, bool border, ImGuiWindowFlags extra = 0) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border ? 1.0f : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, palette().border);
    ImGui::BeginChild(id, size, border ? ImGuiChildFlags_Borders : 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoSavedSettings | extra);
}

void endRegion() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void textCentered(const char* text, const ImVec4& color) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float tw = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - tw) * 0.5f);
    ImGui::TextColored(color, "%s", text);
}

// settings.json 的读取返回 Result：读不到（首次启动/文件损坏）就用默认值，
// 界面不能因为"没有配置文件"而失败。
Settings loadSettings() {
    auto loaded = SettingsStore::load();
    return loaded ? *loaded : Settings{};
}

// 主题强调色以 "#RRGGBB" 存在 Settings::accent 里；v1 的 "teal" 等旧值
// 解析失败 → 回落到内置蓝，不做映射（旧主题体系已下线）。
bool parseHexColor(const std::string& s, ImVec4* out) {
    if (s.size() != 7 || s[0] != '#') {
        return false;
    }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(s[static_cast<size_t>(i + 1)]);
        if (v[i] < 0) {
            return false;
        }
    }
    *out = ImVec4(static_cast<float>(v[0] * 16 + v[1]) / 255.0f,
                  static_cast<float>(v[2] * 16 + v[3]) / 255.0f,
                  static_cast<float>(v[4] * 16 + v[5]) / 255.0f, 1.0f);
    return true;
}

std::string toHexColor(const ImVec4& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  static_cast<int>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<int>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<int>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f + 0.5f));
    return buf;
}

// —— 水盒几何：手动（中心+半长）或轨迹首帧 box（正交/三斜都按三个棱向量）——
struct BoxGeom {
    glm::vec3 origin{0.0f};
    glm::vec3 a1{0.0f};
    glm::vec3 a2{0.0f};
    glm::vec3 a3{0.0f};
};

bool computeBoxGeom(const entity::SceneStructure* scene, bool hasRaw, const glm::vec3& rawOrigin,
                    const glm::vec3& rawA1, const glm::vec3& rawA2, const glm::vec3& rawA3,
                    bool manual, const glm::vec3& center, const glm::vec3& half,
                    BoxGeom* out) {
    if (out == nullptr) {
        return false;
    }
    if (hasRaw) {
        // 从 .mlscene 读回的三斜盒：原样使用三条棱向量
        out->origin = rawOrigin;
        out->a1 = rawA1;
        out->a2 = rawA2;
        out->a3 = rawA3;
        return glm::length(out->a1) + glm::length(out->a2) + glm::length(out->a3) > 1e-3f;
    }
    if (manual) {
        // 轴对齐盒：中心 ± 半长；origin 取最小角，三条棱 = 2×半长
        out->origin = center - half;
        out->a1 = glm::vec3(half.x * 2.0f, 0.0f, 0.0f);
        out->a2 = glm::vec3(0.0f, half.y * 2.0f, 0.0f);
        out->a3 = glm::vec3(0.0f, 0.0f, half.z * 2.0f);
        return half.x > 1e-3f && half.y > 1e-3f && half.z > 1e-3f;
    }
    if (scene == nullptr) {
        return false;
    }
    const auto& insts = scene->instances();
    if (insts.empty() || !insts.front().trajectory ||
        insts.front().trajectory->frames.empty()) {
        return false;
    }
    const auto& b = insts.front().trajectory->frames.front().box;
    out->a1 = glm::vec3(b[0], b[1], b[2]);
    out->a2 = glm::vec3(b[3], b[4], b[5]);
    out->a3 = glm::vec3(b[6], b[7], b[8]);
    if (glm::length(out->a1) + glm::length(out->a2) + glm::length(out->a3) < 1e-3f) {
        return false;   // 无盒信息（纯 PDB 常见）
    }
    // 盒中心 = 场景坐标包围盒中心（盒子通常包住整个体系）
    glm::vec3 lo(FLT_MAX);
    glm::vec3 hi(-FLT_MAX);
    for (const auto& p : scene->coordsSoA().positions) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec3 c = (lo + hi) * 0.5f;
    out->origin = c - (out->a1 + out->a2 + out->a3) * 0.5f;
    return true;
}

} // namespace

Workbench::Workbench() = default;
Workbench::~Workbench() = default;

void Workbench::init(const WorkbenchServices& svc) {
    svc_ = svc;

    // 主题：v2 只认 aurora-light / aurora-dark；旧主题键一律回落到浅色，
    // 避免用户升级后突然进入一个从未见过的配色。
    Settings settings = loadSettings();
    aurora::apply(settings.theme == "aurora-dark");
    // 用户自定义强调色（v2.3）：apply 之后再覆盖，切主题时 apply 内部会保留。
    ImVec4 accentColor;
    if (parseHexColor(settings.accent, &accentColor)) {
        aurora::setAccent(accentColor);
    }

    addModule(makeVisualizeModule());
    addModule(makeSceneModule());
    // v2.8：分子模块已并入「显示」，不再单列（见 modules.cpp renderMoleculeSection）
    addModule(makeEspModule());
    addModule(makeOrbitalModule());
    addModule(makeTrajectoryModule());
    addModule(makeAnnotationModule());
    addModule(makeAnalysisModule());
    addModule(makeLogModule());
    addModule(makeSettingsModule());

    // 时间轴控件（v2.7）：只在轨道数据存在时显示在分子视图下方
    timeAxis_ = std::make_unique<TimeAxis>();
    timeAxis_->setController(svc_.timelineCtl);

    if (findModule(activeId_.c_str()) == nullptr && !modules_.empty()) {
        activeId_ = modules_.front()->id();
    }
    spdlog::info("[workbench] Aurora 工作台就绪：{} 个模块", modules_.size());
}

void Workbench::addModule(std::unique_ptr<IInspectorModule> module) {
    if (module) {
        modules_.push_back(std::move(module));
    }
}

IInspectorModule* Workbench::findModule(const char* id) const {
    for (const auto& m : modules_) {
        if (std::strcmp(m->id(), id) == 0) {
            return m.get();
        }
    }
    return nullptr;
}

float Workbench::fps() const { return (svc_.fps != nullptr) ? *svc_.fps : 0.0f; }

int32_t Workbench::activeRepIndex() const {
    return svc_.repCtl != nullptr ? svc_.repCtl->activeIndex() : 0;
}

void Workbench::setActiveRepIndex(int32_t index) {
    if (svc_.repCtl != nullptr) {
        svc_.repCtl->setActiveIndex(index);
    }
}

bool Workbench::isRepMultiSelected(int32_t id) const {
    return std::find(repMultiSel_.begin(), repMultiSel_.end(), id) != repMultiSel_.end();
}

void Workbench::toggleRepMultiSelected(int32_t id) {
    const auto it = std::find(repMultiSel_.begin(), repMultiSel_.end(), id);
    if (it != repMultiSel_.end()) {
        repMultiSel_.erase(it);
    } else {
        repMultiSel_.push_back(id);
        std::sort(repMultiSel_.begin(), repMultiSel_.end());
    }
    if (svc_.repCtl != nullptr) {
        svc_.repCtl->setActiveIndex(id);
    }
}

engine::Representation* Workbench::activeRep() {
    if (svc_.repCtl == nullptr) {
        return nullptr;
    }
    auto& list = const_cast<std::vector<engine::Representation>&>(
        svc_.repCtl->representations());
    if (list.empty()) {
        return nullptr;
    }
    const int32_t idx = std::clamp<int32_t>(svc_.repCtl->activeIndex(), 0,
                                            static_cast<int32_t>(list.size()) - 1);
    return &list[static_cast<size_t>(idx)];
}

const std::vector<engine::Representation>& Workbench::representations() const {
    static const std::vector<engine::Representation> kEmpty;
    return svc_.repCtl != nullptr ? svc_.repCtl->representations() : kEmpty;
}

void Workbench::notify(const std::string& text) {
    toast_ = text;
    toastAge_ = 0.0f;
    logLine(text);
    spdlog::info("[workbench] {}", text);
}

void Workbench::logLine(const std::string& text) {
    logLines_.push_back(text);
    if (logLines_.size() > 400) {
        logLines_.erase(logLines_.begin(), logLines_.begin() + 100);
    }
}

void Workbench::setModule(const char* id) {
    IInspectorModule* m = findModule(id);
    if (m == nullptr) {
        return;
    }
    if (activeId_ != id) {
        activeId_ = id;
        m->onActivate(*this);
    }
}

const char* Workbench::activeModuleId() const { return activeId_.c_str(); }

void Workbench::setHydrogenHidden(bool on) {
    if (on == hideHydrogen_ || svc_.repCtl == nullptr) {
        hideHydrogen_ = on;
        return;
    }
    hideHydrogen_ = on;
    const auto& reps = svc_.repCtl->representations();
    if (on) {
        savedSelections_.clear();
        for (size_t i = 0; i < reps.size(); ++i) {
            savedSelections_[static_cast<int32_t>(i)] = reps[i].selection;
        }
        for (size_t i = 0; i < reps.size(); ++i) {
            svc_.repCtl->setSelection(static_cast<int32_t>(i), "not element H");
        }
        notify("已隐藏氢原子（选择表达式：not element H）");
    } else {
        for (const auto& kv : savedSelections_) {
            svc_.repCtl->setSelection(kv.first, kv.second);
        }
        savedSelections_.clear();
        notify("已恢复全部原子");
    }
}

void Workbench::frameCamera() {
    if (svc_.cameraCtl != nullptr) {
        svc_.cameraCtl->centerOnScene();
        if (svc_.viewport != nullptr) {
            svc_.viewport->markDirty();
        }
    }
}

void Workbench::resetCamera() {
    if (svc_.cameraCtl != nullptr) {
        svc_.cameraCtl->reset();
        if (svc_.viewport != nullptr) {
            svc_.viewport->markDirty();
        }
    }
}

void Workbench::toggleTheme() {
    aurora::toggleDark();
    Settings s = loadSettings();
    s.theme = aurora::isDark() ? "aurora-dark" : "aurora-light";
    SettingsStore::save(s);
    notify(aurora::isDark() ? "已切换到深色主题" : "已切换到浅色主题（Aurora Lab）");
}

void Workbench::setInspectorWidth(float w) {
    inspectorW_ = std::clamp(w, aurora::metrics::kInspectorMin,
                             aurora::metrics::kInspectorMax);
}

void Workbench::setRailCollapsed(bool on) {
    if (railCollapsed_ == on) {
        return;
    }
    railCollapsed_ = on;
    notify(on ? tr("工作区已收起（Ctrl+B 展开）") : tr("工作区已展开"));
}

void Workbench::toggleRailCollapsed() {
    setRailCollapsed(!railCollapsed_);
}

bool Workbench::hasDocument() const {
    return svc_.ioCtl != nullptr && !svc_.ioCtl->currentSourcePath().empty();
}

std::string Workbench::documentName() const {
    if (!hasDocument()) {
        return "未命名场景";
    }
    const std::string& p = svc_.ioCtl->currentSourcePath();
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

int Workbench::totalAtoms() const {
    if (svc_.scene == nullptr) {
        return 0;
    }
    // v1 的 MoleculeInstance::globalAtomIds 只在拾取回查时才填，普通加载后为空。
    // 所以原子数必须从结构池里数（与 DataManager::atomCount 口径一致）。
    const auto& structures = svc_.scene->structures();
    const auto& instances = svc_.scene->instances();
    int total = 0;
    for (const auto& inst : instances) {
        if (inst.structureIndex < structures.size()) {
            total += static_cast<int>(structures[inst.structureIndex].elements.size());
        }
    }
    return total;
}

int Workbench::totalMolecules() const {
    return svc_.data != nullptr ? static_cast<int>(svc_.data->moleculeCount()) : 0;
}

// -----------------------------------------------------------------------------
// 场景绘制 / 标注（Publication 风格）
// -----------------------------------------------------------------------------
glm::vec3 Workbench::moleculeCenter(size_t moleculeIndex) const {
    if (svc_.scene == nullptr) {
        return glm::vec3(0.0f);
    }
    const auto& instances = svc_.scene->instances();
    if (moleculeIndex >= instances.size()) {
        return glm::vec3(0.0f);
    }
    const auto& inst = instances[moleculeIndex];
    if (inst.structureIndex >= svc_.scene->structures().size()) {
        return glm::vec3(0.0f);
    }
    const auto& st = svc_.scene->structures()[inst.structureIndex];
    const uint32_t base = svc_.scene->globalAtomBase(static_cast<uint32_t>(moleculeIndex));
    const auto& pos = svc_.scene->coordsSoA().positions;
    glm::vec3 c(0.0f);
    size_t n = 0;
    for (size_t i = 0; i < st.elements.size(); ++i) {
        if (static_cast<size_t>(base) + i < pos.size()) {
            c += pos[static_cast<size_t>(base) + i];
            ++n;
        }
    }
    return n > 0 ? c / static_cast<float>(n) : glm::vec3(0.0f);
}

glm::vec3 Workbench::viewCenter() const {
    if (svc_.viewport == nullptr) {
        return glm::vec3(0.0f);
    }
    const engine::OrbitCamera* cam = svc_.viewport->getCamera();
    return cam != nullptr ? cam->target : glm::vec3(0.0f);
}

void Workbench::addLabel(const std::string& text, const glm::vec3& pos) {
    SceneLabel l;
    l.text = text;
    l.pos = pos;
    l.size = 18.0f;
    labels_.push_back(l);
}

void Workbench::labelMolecules(size_t first, size_t count) {
    const size_t total = svc_.data != nullptr ? svc_.data->moleculeCount() : 0;
    const size_t end = std::min(total, first + count);
    for (size_t i = first; i < end; ++i) {
        SceneLabel l;
        l.text = svc_.data->moleculeName(i);
        if (l.text.empty()) {
            l.text = "mol_" + std::to_string(i);
        }
        l.pos = moleculeCenter(i);
        l.size = 14.0f;
        l.color = ImVec4(1.0f, 0.92f, 0.55f, 1.0f);
        labels_.push_back(l);
    }
    notify("已为 " + std::to_string(end - first) + " 个分子添加名称标注");
}

void Workbench::clearAnnotations() {
    labels_.clear();
    arrows_.clear();
    showBox_ = false;
    notify("已清除全部标注");
}

void Workbench::applyPublicationStyle() {
    if (svc_.renderCtl != nullptr) {
        svc_.renderCtl->setBackgroundColor(0.0f, 0.0f, 0.0f);
        svc_.renderCtl->setShadowsEnabled(false);
    }
    // 平光材质：高环境光 + 低高光，接近 VMD「AOChalky / 黑底卡通」的观感
    if (svc_.repCtl != nullptr) {
        auto& reps = const_cast<std::vector<engine::Representation>&>(
            svc_.repCtl->representations());
        for (size_t i = 0; i < reps.size(); ++i) {
            const auto& r = reps[i];
            const float ambient[4] = {0.88f, 0.88f, 0.88f, 1.0f};
            const float diffuse[4] = {r.diffuse[0], r.diffuse[1], r.diffuse[2], 1.0f};
            const float specular[4] = {0.05f, 0.05f, 0.05f, 1.0f};
            const float shininess = 8.0f;
            const float metallic = 0.0f;
            const float roughness = 0.9f;
            svc_.repCtl->setMaterial(static_cast<int32_t>(i), -1, ambient, diffuse, specular,
                                     shininess, metallic, roughness, r.opacity);
        }
    }
    notify("已应用出版风格：黑底 + 平光 + 关阴影（文字/箭头用「标注」页添加）");
}

// 示意图风格：参考 TCBG 光合体系图的观感——黑底、平光、强轮廓、无阴影、无距离淡出。
// 与「出版风格」的差别：轮廓更重（0.8）、环绕漫反射更大（0.55）、彻底关闭景深淡出，
// 使远处的卡通螺旋与近处一样清晰（示意图不追求空气透视）。
void Workbench::applyDiagramStyle() {
    if (svc_.renderCtl != nullptr) {
        svc_.renderCtl->setBackgroundColor(0.0f, 0.0f, 0.0f);
        svc_.renderCtl->setShadowsEnabled(false);
        engine::RenderStyle style;
        style.wrap = 0.55f;
        style.rim = 0.0f;
        style.outline = 0.80f;
        style.depthCue = 0.0f;
        svc_.renderCtl->setRenderStyle(style);
    }
    if (svc_.repCtl != nullptr) {
        auto& reps = const_cast<std::vector<engine::Representation>&>(
            svc_.repCtl->representations());
        for (size_t i = 0; i < reps.size(); ++i) {
            const auto& r = reps[i];
            const float ambient[4] = {0.95f, 0.95f, 0.95f, 1.0f};
            const float diffuse[4] = {r.diffuse[0], r.diffuse[1], r.diffuse[2], 1.0f};
            const float specular[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            svc_.repCtl->setMaterial(static_cast<int32_t>(i), -1, ambient, diffuse, specular,
                                     4.0f, 0.0f, 1.0f, r.opacity);
        }
    }
    showBox_ = true;
    watermark_.visible = true;
    if (watermark_.text.empty()) {
        watermark_.text = "MolecuLens";
    }
    notify("已应用示意图风格：黑底 + 强轮廓 + 平光（可继续加标签/箭头/水盒）");
}

// 把盒中心/半长设为当前场景包围盒（"适配场景"按钮）
void Workbench::fitBoxToScene() {
    if (svc_.scene == nullptr) {
        return;
    }
    glm::vec3 lo(FLT_MAX);
    glm::vec3 hi(-FLT_MAX);
    for (const auto& p : svc_.scene->coordsSoA().positions) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    if (lo.x > hi.x) {
        return;   // 空场景
    }
    boxCenter_ = (lo + hi) * 0.5f;
    // 留 5% 余量：盒线贴着最外层原子不好看
    boxHalf_ = glm::max((hi - lo) * 0.5f * 1.05f, glm::vec3(1.0f));
    boxManual_ = true;
    showBox_ = true;
    notify("水盒已按场景包围盒适配");
}

// 把标注写入 engine 快照（每帧一次）：导出 PNG 的栅格器只读这份快照，
// 不需要认识 Workbench / ImGui。
void Workbench::publishOverlaySnapshot() {
    auto& snap = engine::scene_overlay::mutableSnapshot();
    snap = engine::scene_overlay::Snapshot{};
    if (showBox_) {
        glm::vec3 origin;
        glm::vec3 a1;
        glm::vec3 a2;
        glm::vec3 a3;
        if (currentBox(&origin, &a1, &a2, &a3)) {
            snap.box.visible = true;
            snap.box.origin = origin;
            snap.box.a1 = a1;
            snap.box.a2 = a2;
            snap.box.a3 = a3;
            snap.box.color[0] = boxColor_.x;
            snap.box.color[1] = boxColor_.y;
            snap.box.color[2] = boxColor_.z;
            snap.box.color[3] = boxColor_.w;
            snap.box.lineWidth = boxLineWidth_;
        }
    }
    snap.labels.reserve(labels_.size());
    for (const auto& l : labels_) {
        engine::scene_overlay::Label out;
        out.text = l.text;
        out.pos = l.pos;
        out.color[0] = l.color.x;
        out.color[1] = l.color.y;
        out.color[2] = l.color.z;
        out.color[3] = l.color.w;
        out.pixelSize = l.size;
        out.leader = l.leader;
        out.visible = l.visible;
        snap.labels.push_back(std::move(out));
    }
    snap.arrows.reserve(arrows_.size());
    for (const auto& a : arrows_) {
        engine::scene_overlay::Arrow out;
        out.from = a.from;
        out.to = a.to;
        out.color[0] = a.color.x;
        out.color[1] = a.color.y;
        out.color[2] = a.color.z;
        out.color[3] = a.color.w;
        out.width = a.width;
        out.headScale = a.headScale;
        out.doubleHead = a.doubleHead;
        out.style = a.style == 1 ? engine::scene_overlay::ArrowStyle::Zigzag
                                 : engine::scene_overlay::ArrowStyle::Plain;
        out.visible = a.visible;
        snap.arrows.push_back(std::move(out));
    }
    snap.watermark.visible = watermark_.visible;
    snap.watermark.text = watermark_.text;
    snap.watermark.color[0] = watermark_.color.x;
    snap.watermark.color[1] = watermark_.color.y;
    snap.watermark.color[2] = watermark_.color.z;
    snap.watermark.color[3] = watermark_.color.w;
    snap.watermark.scale = watermark_.scale;
    snap.watermark.anchor = static_cast<engine::scene_overlay::Anchor>(
        std::clamp(watermark_.anchor, 0, 3));
}

// 当前水盒几何：文件读回的三斜盒优先，其次手动中心+半长，最后轨迹首帧 box
bool Workbench::currentBox(glm::vec3* origin, glm::vec3* a1, glm::vec3* a2,
                           glm::vec3* a3) const {
    BoxGeom geom;
    if (!computeBoxGeom(svc_.scene, boxHasRaw_, boxRawOrigin_, boxRawA1_, boxRawA2_,
                        boxRawA3_, boxManual_, boxCenter_, boxHalf_, &geom)) {
        return false;
    }
    if (origin != nullptr) {
        *origin = geom.origin;
    }
    if (a1 != nullptr) {
        *a1 = geom.a1;
    }
    if (a2 != nullptr) {
        *a2 = geom.a2;
    }
    if (a3 != nullptr) {
        *a3 = geom.a3;
    }
    return true;
}

// —— 标注持久化：Workbench 成员 ↔ .mlscene 的纯数据（颜色/坐标逐个拷）——
persist::mlscene::Annotations Workbench::exportAnnotations() const {
    persist::mlscene::Annotations out;
    glm::vec3 origin;
    glm::vec3 a1;
    glm::vec3 a2;
    glm::vec3 a3;
    if (showBox_ && currentBox(&origin, &a1, &a2, &a3)) {
        out.box.visible = true;
        out.box.origin = {origin.x, origin.y, origin.z};
        out.box.axes = {a1.x, a1.y, a1.z, a2.x, a2.y, a2.z, a3.x, a3.y, a3.z};
        out.box.color = {boxColor_.x, boxColor_.y, boxColor_.z, boxColor_.w};
        out.box.lineWidth = boxLineWidth_;
    }
    out.labels.reserve(labels_.size());
    for (const auto& l : labels_) {
        persist::mlscene::AnnotationLabel label;
        label.text = l.text;
        label.pos = {l.pos.x, l.pos.y, l.pos.z};
        label.color = {l.color.x, l.color.y, l.color.z, l.color.w};
        label.pixelSize = l.size;
        label.leader = l.leader;
        label.visible = l.visible;
        out.labels.push_back(std::move(label));
    }
    out.arrows.reserve(arrows_.size());
    for (const auto& a : arrows_) {
        persist::mlscene::AnnotationArrow arrow;
        arrow.from = {a.from.x, a.from.y, a.from.z};
        arrow.to = {a.to.x, a.to.y, a.to.z};
        arrow.color = {a.color.x, a.color.y, a.color.z, a.color.w};
        arrow.width = a.width;
        arrow.headScale = a.headScale;
        arrow.doubleHead = a.doubleHead;
        arrow.style = a.style;
        arrow.visible = a.visible;
        out.arrows.push_back(std::move(arrow));
    }
    if (watermark_.visible && !watermark_.text.empty()) {
        out.watermark.visible = true;
        out.watermark.text = watermark_.text;
        out.watermark.color = {watermark_.color.x, watermark_.color.y, watermark_.color.z,
                               watermark_.color.w};
        out.watermark.scale = watermark_.scale;
        out.watermark.anchor = watermark_.anchor;
    }
    return out;
}

void Workbench::importAnnotations(const persist::mlscene::Annotations& an) {
    labels_.clear();
    arrows_.clear();
    watermark_ = SceneWatermark{};
    showBox_ = an.box.visible;
    boxColor_ = ImVec4(an.box.color[0], an.box.color[1], an.box.color[2], an.box.color[3]);
    boxLineWidth_ = an.box.lineWidth;
    if (an.box.visible) {
        // 文件里存的是实际几何（可能三斜）：原样保留，界面按"手动"显示；
        // 用户一旦改中心/半长，setBoxCenter/setBoxHalf 会清掉 raw 并转为轴对齐。
        boxHasRaw_ = true;
        boxRawOrigin_ = glm::vec3(an.box.origin[0], an.box.origin[1], an.box.origin[2]);
        boxRawA1_ = glm::vec3(an.box.axes[0], an.box.axes[1], an.box.axes[2]);
        boxRawA2_ = glm::vec3(an.box.axes[3], an.box.axes[4], an.box.axes[5]);
        boxRawA3_ = glm::vec3(an.box.axes[6], an.box.axes[7], an.box.axes[8]);
        boxManual_ = true;
        boxCenter_ = boxRawOrigin_ + (boxRawA1_ + boxRawA2_ + boxRawA3_) * 0.5f;
        boxHalf_ = glm::vec3(glm::length(boxRawA1_) * 0.5f, glm::length(boxRawA2_) * 0.5f,
                             glm::length(boxRawA3_) * 0.5f);
    } else {
        boxHasRaw_ = false;
    }
    for (const auto& l : an.labels) {
        SceneLabel label;
        label.text = l.text;
        label.pos = glm::vec3(l.pos[0], l.pos[1], l.pos[2]);
        label.color = ImVec4(l.color[0], l.color[1], l.color[2], l.color[3]);
        label.size = l.pixelSize;
        label.leader = l.leader;
        label.visible = l.visible;
        labels_.push_back(std::move(label));
    }
    for (const auto& a : an.arrows) {
        SceneArrow arrow;
        arrow.from = glm::vec3(a.from[0], a.from[1], a.from[2]);
        arrow.to = glm::vec3(a.to[0], a.to[1], a.to[2]);
        arrow.color = ImVec4(a.color[0], a.color[1], a.color[2], a.color[3]);
        arrow.width = a.width;
        arrow.headScale = a.headScale;
        arrow.doubleHead = a.doubleHead;
        arrow.style = a.style == 1 ? 1 : 0;
        arrow.visible = a.visible;
        arrows_.push_back(std::move(arrow));
    }
    if (an.watermark.visible) {
        watermark_.visible = true;
        watermark_.text = an.watermark.text;
        watermark_.color = ImVec4(an.watermark.color[0], an.watermark.color[1],
                                  an.watermark.color[2], an.watermark.color[3]);
        watermark_.scale = an.watermark.scale;
        watermark_.anchor = std::clamp(an.watermark.anchor, 0, 3);
    }
    if (!an.empty()) {
        notify("已载入场景标注（水盒/标签/箭头/水印）");
    }
}

bool Workbench::projectToViewport(const glm::vec3& world, const ImVec2& imgMin,
                                  const ImVec2& imgMax, ImVec2* out) const {
    if (svc_.viewport == nullptr) {
        return false;
    }
    const uint32_t rw = svc_.viewport->displayWidth();
    const uint32_t rh = svc_.viewport->displayHeight();
    if (rw < 2 || rh < 2) {
        return false;
    }
    const engine::OrbitCamera* cam = svc_.viewport->getCamera();
    if (cam == nullptr) {
        return false;
    }
    const glm::mat4 vp = cam->viewProjMatrix(static_cast<float>(rw) / static_cast<float>(rh));
    float px = 0.0f;
    float py = 0.0f;
    if (!engine::esp_overlay::projectToPixel(vp, world, static_cast<int>(rw),
                                            static_cast<int>(rh), px, py)) {
        return false;
    }
    const float sx = (imgMax.x - imgMin.x) / static_cast<float>(rw);
    const float sy = (imgMax.y - imgMin.y) / static_cast<float>(rh);
    out->x = imgMin.x + px * sx;
    out->y = imgMin.y + py * sy;
    return true;
}

void Workbench::drawSceneOverlay(const ImVec2& imgMin, const ImVec2& imgMax) {
    if (svc_.scene == nullptr || svc_.viewport == nullptr) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = imgMin;
    const ImVec2 max = imgMax;
    auto project = [&](const glm::vec3& p, ImVec2* out) {
        return projectToViewport(p, min, max, out);
    };

    // —— 1. 水盒 / 周期盒（文件读回的三斜盒 / 手动中心+半长 / 轨迹首帧 box）——
    if (showBox_) {
        glm::vec3 origin;
        glm::vec3 a1;
        glm::vec3 a2;
        glm::vec3 a3;
        if (currentBox(&origin, &a1, &a2, &a3)) {
            glm::vec3 corner[8];
            for (int i = 0; i < 8; ++i) {
                corner[i] = origin + ((i & 1) ? a1 : glm::vec3(0.0f)) +
                            ((i & 2) ? a2 : glm::vec3(0.0f)) +
                            ((i & 4) ? a3 : glm::vec3(0.0f));
            }
            static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                              {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                              {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            const ImU32 bc = ImGui::GetColorU32(boxColor_);
            for (const auto& e : kEdges) {
                ImVec2 p0;
                ImVec2 p1;
                if (project(corner[e[0]], &p0) && project(corner[e[1]], &p1)) {
                    dl->AddLine(p0, p1, bc, std::max(1.0f, boxLineWidth_));
                }
            }
        }
    }

    // —— 2. 箭头（普通 / 波浪"光"箭头；可选双头）——
    for (const auto& a : arrows_) {
        if (!a.visible) {
            continue;
        }
        ImVec2 p0;
        ImVec2 p1;
        if (!project(a.from, &p0) || !project(a.to, &p1)) {
            continue;
        }
        const ImU32 col = ImGui::GetColorU32(a.color);
        const float dx = p1.x - p0.x;
        const float dy = p1.y - p0.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 6.0f) {
            continue;
        }
        const float ux = dx / len;
        const float uy = dy / len;
        const float head = std::min(26.0f, len * 0.35f) * std::max(0.4f, a.headScale);
        const float width = std::max(1.0f, a.width);

        // 画箭头头部（tip 在 tipEnd，方向 (hx,hy)）
        const auto drawHead = [&](const ImVec2& tip, float hx, float hy) {
            const ImVec2 h1(tip.x - hx * head - hy * head * 0.45f,
                            tip.y - hy * head + hx * head * 0.45f);
            const ImVec2 h2(tip.x - hx * head + hy * head * 0.45f,
                            tip.y - hy * head - hx * head * 0.45f);
            dl->AddTriangleFilled(tip, h1, h2, col);
        };

        if (a.style == 1) {
            // 波浪：沿弦方向的正弦折线（"光子"箭头）；波长/采样与导出栅格器一致
            const float amp = width * 1.8f;
            const float wave = std::max(14.0f, 22.0f);
            const int steps = std::max(24, static_cast<int>(len / 1.2f));
            const float nx = -uy;
            const float ny = ux;
            ImVec2 prev = p0;
            for (int i = 1; i <= steps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                const float off = (i == steps) ? 0.0f
                                               : std::sin(t * len / wave * 6.2831853f) * amp;
                const ImVec2 cur(p0.x + dx * t + nx * off, p0.y + dy * t + ny * off);
                dl->AddLine(prev, cur, col, width);
                prev = cur;
            }
            drawHead(p1, ux, uy);
            if (a.doubleHead) {
                drawHead(p0, -ux, -uy);
            }
        } else {
            dl->AddLine(p0, ImVec2(p1.x - ux * head * 0.85f, p1.y - uy * head * 0.85f), col,
                        width);
            drawHead(p1, ux, uy);
            if (a.doubleHead) {
                dl->AddLine(ImVec2(p0.x + ux * head * 0.85f, p0.y + uy * head * 0.85f), p1,
                            col, width);
                drawHead(p0, -ux, -uy);
            }
        }
    }

    // —— 3. 文字标签（带深色描边，黑底/亮表面都清晰）——
    ImFont* font = UITheme::fontTitle() != nullptr ? UITheme::fontTitle()
                                                   : ImGui::GetFont();
    if (font == nullptr) {
        return;
    }
    for (const auto& l : labels_) {
        if (!l.visible || l.text.empty()) {
            continue;
        }
        ImVec2 p;
        if (!project(l.pos, &p)) {
            continue;
        }
        const float fs = std::clamp(l.size, 9.0f, 72.0f);
        if (l.leader) {
            dl->AddCircleFilled(p, 3.0f, ImGui::GetColorU32(l.color));
        }
        const ImU32 fg = ImGui::GetColorU32(l.color);
        const ImU32 outline = IM_COL32(0, 0, 0, 210);
        const ImVec2 tp(p.x + 8.0f, p.y - fs * 0.6f);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0) {
                    dl->AddText(font, fs, ImVec2(tp.x + dx, tp.y + dy), outline,
                                l.text.c_str());
                }
            }
        }
        dl->AddText(font, fs, tp, fg, l.text.c_str());
    }

    // —— 4. 水印（图角落的大号半透明文字）——
    if (watermark_.visible && !watermark_.text.empty()) {
        const float fs = std::clamp(watermark_.scale * (max.y - min.y), 10.0f, 220.0f);
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, watermark_.text.c_str());
        const float margin = (max.x - min.x) * 0.03f;
        ImVec2 tp(min.x + margin, max.y - margin - ts.y);
        switch (watermark_.anchor) {
            case 1:   // 左下
                tp = ImVec2(min.x + margin, max.y - margin - ts.y);
                break;
            case 2:   // 居中
                tp = ImVec2((min.x + max.x - ts.x) * 0.5f,
                            (min.y + max.y - ts.y) * 0.5f);
                break;
            case 3:   // 右上
                tp = ImVec2(max.x - margin - ts.x, min.y + margin);
                break;
            case 0:   // 右下（默认）
            default:
                tp = ImVec2(max.x - margin - ts.x, max.y - margin - ts.y);
                break;
        }
        dl->AddText(font, fs, tp, ImGui::GetColorU32(watermark_.color),
                    watermark_.text.c_str());
    }
}

// -----------------------------------------------------------------------------
// 快捷样式
// -----------------------------------------------------------------------------
void Workbench::applyPreset(int preset) {
    if (svc_.repCtl == nullptr) {
        return;
    }
    using engine::ColorScheme;
    using engine::RepresentationType;
    auto& list = const_cast<std::vector<engine::Representation>&>(
        svc_.repCtl->representations());

    auto setAll = [&](RepresentationType type, ColorScheme scheme, float opacity,
                      int material) {
        if (list.empty()) {
            svc_.repCtl->addRepresentation("表示 1", type, "", scheme);
            list = const_cast<std::vector<engine::Representation>&>(
                svc_.repCtl->representations());
        }
        for (size_t i = 0; i < list.size(); ++i) {
            const int32_t id = static_cast<int32_t>(i);
            svc_.repCtl->setType(id, type);
            svc_.repCtl->setColorScheme(id, scheme);
            const auto& p = engine::materialPresetAt(material);
            svc_.repCtl->setMaterialPreset(
                id, material, p.ambient, p.diffuse, p.specular, p.shininess,
                p.metallic, p.roughness, opacity);
        }
    };

    switch (preset) {
        case kSobArt: {
            setAll(RepresentationType::BallAndStick, ColorScheme::ElementCPK, 1.0f, 0);
            if (svc_.renderCtl != nullptr) {
                svc_.renderCtl->setBackgroundColor(0.09f, 0.11f, 0.16f);
            }
            notify("样式 · sob-art：球棍 + CPK + 深背景");
            break;
        }
        case kIBOview: {
            setAll(RepresentationType::BallAndStick, ColorScheme::ElementCPK, 1.0f, 0);
            // 重复点同一个快捷样式不能把外壳越叠越多：先清掉上次加的 IBOview 外壳
            for (int32_t i = static_cast<int32_t>(list.size()) - 1; i >= 0; --i) {
                if (list[static_cast<size_t>(i)].name == "IBOview 表面") {
                    svc_.repCtl->removeRepresentation(i);
                }
            }
            list = const_cast<std::vector<engine::Representation>&>(
                svc_.repCtl->representations());
            // IBOview 的标志是"外面一层半透明表面 + 内部球棍"
            const int32_t surfId = svc_.repCtl->addRepresentation(
                "IBOview 表面", RepresentationType::QuickSurf, "", ColorScheme::Uniform);
            if (surfId >= 0) {
                svc_.repCtl->setSystemSurface(surfId, true);
                svc_.repCtl->setSurfIsovalue(surfId, 0.35f);
                svc_.repCtl->setMaterialPreset(surfId, 1,
                                               engine::kMaterialPresets[1].ambient,
                                               engine::kMaterialPresets[1].diffuse,
                                               engine::kMaterialPresets[1].specular,
                                               engine::kMaterialPresets[1].shininess,
                                               engine::kMaterialPresets[1].metallic,
                                               engine::kMaterialPresets[1].roughness, 0.35f);
            }
            if (svc_.renderCtl != nullptr) {
                svc_.renderCtl->setBackgroundColor(1.0f, 1.0f, 1.0f);
            }
            notify("样式 · IBOview：球棍 + 半透明外壳");
            break;
        }
        case kHoukMol: {
            setAll(RepresentationType::Stick, ColorScheme::ElementCPK, 1.0f, 0);
            if (svc_.renderCtl != nullptr) {
                svc_.renderCtl->setBackgroundColor(0.05f, 0.06f, 0.09f);
            }
            notify("样式 · HoukMol：棍状 + CPK");
            break;
        }
        case kIQmol: {
            setAll(RepresentationType::BallAndStick, ColorScheme::ElementCPK, 1.0f, 2);
            if (svc_.renderCtl != nullptr) {
                svc_.renderCtl->setBackgroundColor(1.0f, 1.0f, 1.0f);
            }
            notify("样式 · IQmol：球棍 + 光泽材质");
            break;
        }
        case kClearStyle: {
            for (int32_t i = static_cast<int32_t>(list.size()) - 1; i >= 0; --i) {
                svc_.repCtl->removeRepresentation(i);
            }
            svc_.repCtl->addRepresentation("表示 1", RepresentationType::BallAndStick,
                                           "", ColorScheme::ElementCPK);
            svc_.repCtl->setActiveIndex(0);
            preset_ = -1;
            notify("已清空样式，恢复默认球棍表示");
            break;
        }
        default:
            return;
    }
    preset_ = preset;
    if (svc_.viewport != nullptr) {
        svc_.viewport->markDirty();
    }
}

// -----------------------------------------------------------------------------
// 文件动作
// -----------------------------------------------------------------------------

namespace {

// "分子结构文件"一组的通配符串（*.pdb;*.ent;*.gro;*.sdf;*.mol;*.cube;*.cub）。
// 为什么从 ParserRegistry 取而不是写死：新增解析器时只需注册 reader，对话框
// 自动跟着变 —— 否则就会出现"解析器能读、对话框里却看不到该类文件"的错配
//（正是用户报的 bug：过滤器只写了 pdb/gro/sdf/cube，漏了 .ent/.mol/.cub）。
std::string structureExtensionPatterns() {
    std::string list;
    for (const auto& ext : entity::ParserRegistry::defaultRegistry().allExtensions()) {
        if (!list.empty()) {
            list.push_back(';');
        }
        list += "*.";
        list += ext;
    }
    return list;
}

// 拼 Win32 打开对话框过滤器（"说明\0通配\0…\0\0"）。
// ⚠ 不能写成 `std::string f = "说明\0通配\0…"`：从 const char* 构造会在第一个
//   NUL 处截断，对话框只剩说明、没有通配符 -> 文件列表为空。
std::string openDialogFilter() {
    const std::string exts = structureExtensionPatterns();
    std::string f;
    const auto item = [&f](const std::string& desc, const std::string& pat) {
        f += desc;
        f.push_back('\0');
        f += pat;
        f.push_back('\0');
    };
    item(std::string(tr("分子结构文件")) + " (" + exts + ")", exts);
    item(tr("MolecuLens 场景 (*.mlscene)"), "*.mlscene");
    item(tr("所有文件 (*.*)"), "*.*");
    return f;   // 末尾单 NUL + std::string 自带终止符 = 双 NUL
}

} // namespace

void Workbench::openFileDialog() {
    if (!svc_.dlgOpen) {
        notify("打开文件对话框不可用");
        return;
    }
    const std::string filter = openDialogFilter();
    // defExt = nullptr：这里不替用户补扩展名（选中 .mlscene/.cube 时补 .pdb 会写坏路径）
    const std::string path = svc_.dlgOpen(filter.c_str(), nullptr);
    if (!path.empty() && svc_.ioCtl != nullptr) {
        svc_.ioCtl->openFile(path);
    }
}

std::string Workbench::chooseFile(const char* filter, const char* defExt) {
    if (!svc_.dlgOpen) {
        return {};
    }
    return svc_.dlgOpen(filter, defExt);
}

void Workbench::saveSceneDialog() {
    if (svc_.ioCtl == nullptr) {
        notify("场景未就绪，无法保存");
        return;
    }
    if (!svc_.dlgSave) {
        notify("保存对话框不可用");
        return;
    }
    // 沿用上次选择（settings.json 顶层 sceneSaveMode，"linked"/"embedded"）。
    // 存稳定字符串而非枚举序号：日后新增模式也不会让旧配置指向错误模式。
    auto loaded = SettingsStore::load();
    if (loaded) {
        saveSceneMode_ = IoController::modeFromKey(loaded.value().sceneSaveMode) ==
                                 SceneSaveMode::Embedded
                             ? 1
                             : 0;
    }
    showSaveScene_ = true;   // 先选模式，再弹系统"另存为"（见 renderSaveSceneModal）
}

// 确认模式后：系统另存为 -> 保存 -> 记忆选择。
void Workbench::doSaveScene(int mode) {
    if (!svc_.dlgSave || svc_.ioCtl == nullptr) {
        return;
    }
    const auto saveMode = (mode == 1) ? SceneSaveMode::Embedded
                                      : SceneSaveMode::Linked;
    // trItems 逐项 tr() 后用 '\0' 重连 —— 直接写字面量会在第一个 NUL 截断过滤器。
    const std::string filter = trItems(
        {"MolecuLens 场景 (*.mlscene)", "*.mlscene", "所有文件 (*.*)", "*.*"});
    const std::string path = svc_.dlgSave("scene.mlscene", filter.c_str(), "mlscene");
    if (path.empty()) {
        return;
    }
    // 记忆选择（load -> 改 -> save：不持有第二份设置状态）
    {
        Settings settings;
        auto loaded = SettingsStore::load();
        if (loaded) {
            settings = loaded.value();
        }
        settings.sceneSaveMode = IoController::modeKey(saveMode);
        if (!SettingsStore::save(settings)) {
            spdlog::warn("[workbench] sceneSaveMode 保存失败");
        }
    }
    if (svc_.ioCtl->saveScene(path, saveMode)) {
        notify((mode == 1 ? "场景已保存（嵌入式）：" : "场景已保存（链接式）：") + path);
    } else {
        notify("保存失败，详见日志");
    }
}

void Workbench::importTrajectoryDialog() {
    if (!svc_.dlgOpen) {
        return;
    }
    const std::string filter = trItems(
        {"轨迹 (*.dcd;*.xtc;*.trr)", "*.dcd;*.xtc;*.trr", "所有文件 (*.*)", "*.*"});
    const std::string path = svc_.dlgOpen(filter.c_str(), nullptr);
    if (!path.empty() && svc_.ioCtl != nullptr) {
        if (svc_.ioCtl->importTrajectory(path)) {
            notify("轨迹已导入");
        } else {
            notify("轨迹导入失败（原子数需与结构一致）");
        }
    }
}

void Workbench::exportPngDialog() { showExport_ = true; }

// -----------------------------------------------------------------------------
// 主渲染
// -----------------------------------------------------------------------------
void Workbench::render() {
    // 首次帧：把焦点交给视口，避免一进来焦点落在某个输入框上吞掉快捷键
    if (firstFrame_) {
        firstFrame_ = false;
        ImGui::SetNextFrameWantCaptureKeyboard(false);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin = vp->Pos;
    const float W = vp->Size.x;
    const float H = vp->Size.y;

    // 光明模式默认白底（v2.8）：主题明暗切换时给分子画面套上对应背景。
    // 只同步一次（缓存判定），之后用户在「背景」里自选的颜色不会被每帧覆盖。
    if (svc_.renderCtl != nullptr &&
        static_cast<int>(aurora::isDark() ? 1 : 0) != themeDarkCached_) {
        themeDarkCached_ = aurora::isDark() ? 1 : 0;
        if (themeDarkCached_ == 1) {
            svc_.renderCtl->setBackgroundColor(0.09f, 0.11f, 0.16f);
        } else {
            svc_.renderCtl->setBackgroundColor(1.0f, 1.0f, 1.0f);
        }
    }

    toastAge_ += ImGui::GetIO().DeltaTime;

    // —— 全局快捷键（输入框聚焦时不抢键）——
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            openFileDialog();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            saveSceneDialog();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            exportPngDialog();
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
            setModule("log");
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) {
            toggleRailCollapsed();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            frameCamera();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            resetCamera();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
            showHelp_ = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && svc_.timelineCtl != nullptr) {
            if (svc_.timelineCtl->playing()) {
                svc_.timelineCtl->pause();
            } else {
                svc_.timelineCtl->play();
            }
        }
        if (svc_.timelineCtl != nullptr) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
                svc_.timelineCtl->step(-1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
                svc_.timelineCtl->step(1);
            }
        }
        // 快捷条只保留 sob-art 一个样式预设（IBOview/HoukMol/IQmol 走「样式」菜单），
        // 因此数字键只绑定 1 = sob-art。
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            applyPreset(kSobArt);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
            setHydrogenHidden(!hideHydrogen_);
        }
    }

    const float kPad = aurora::metrics::kPad;
    const float topH = aurora::metrics::kTitleBar + aurora::metrics::kMenuBar +
                       aurora::metrics::kFileBar;
    const float bodyY = origin.y + topH + 8.0f;
    const float statusY = origin.y + H - aurora::metrics::kStatusBar - 6.0f;
    const float quickY = statusY - aurora::metrics::kQuickBar - 8.0f;
    const float bodyH = quickY - bodyY - 8.0f;
    const float railW = railCollapsed_ ? aurora::metrics::kRailCollapsed
                                       : aurora::metrics::kRail;
    const float bodyX = origin.x + kPad;
    const float bodyW = W - kPad * 2.0f;
    // 检查器宽度：默认更宽，且可在 kInspectorMin..min(Max, 62% 宽) 之间拖动
    // v2.9：分子画面默认窄一些（约与高度同宽），并且**不小于 0.9:1**——
    // 再窄下去分子会被挤成竖条。检查器吃掉剩余宽度，所以四周仍然没有空白。
    const float inspMaxByAspect = bodyW - railW - 24.0f - bodyH * 0.9f;
    const float inspMaxW = std::min(std::clamp(bodyW * 0.66f, aurora::metrics::kInspectorMin,
                                               aurora::metrics::kInspectorMax),
                                    std::max(inspMaxByAspect, aurora::metrics::kInspectorMin));
    const float inspW = std::clamp(inspectorW_, aurora::metrics::kInspectorMin, inspMaxW);
    const float vpX = bodyX + railW + 8.0f;
    const float inspX = bodyX + bodyW - inspW;
    const float vpW = std::max(inspX - vpX - 8.0f, 240.0f);

    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette().bg);
    ImGui::Begin("##aurora_root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus);

    renderTitleBar(origin, ImVec2(W, aurora::metrics::kTitleBar));
    renderMenuBar(ImVec2(origin.x, origin.y + aurora::metrics::kTitleBar),
                  ImVec2(W, aurora::metrics::kMenuBar));
    renderFileBar(ImVec2(origin.x, origin.y + aurora::metrics::kTitleBar +
                                        aurora::metrics::kMenuBar),
                  ImVec2(W, aurora::metrics::kFileBar));
    renderRail(ImVec2(bodyX, bodyY), ImVec2(railW, bodyH));
    renderViewport(ImVec2(vpX, bodyY), ImVec2(vpW, bodyH));
    renderInspector(ImVec2(inspX, bodyY), ImVec2(inspW, bodyH));

    {
        static int layoutLogs = 0;
        if (layoutLogs < 2 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow)) {
            ++layoutLogs;
            spdlog::info("[ui] layout: W={} bodyW={} railW={} inspW={} vpW={} inspX={} scale={}",
                         W, bodyW, railW, inspW, vpW, inspX,
                         ImGui::GetIO().DisplayFramebufferScale.x);
        }
    }
    renderQuickBar(ImVec2(bodyX, quickY), ImVec2(bodyW, aurora::metrics::kQuickBar));
    renderStatusBar(ImVec2(bodyX, statusY), ImVec2(bodyW, aurora::metrics::kStatusBar));

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    renderToast();
    renderModals();

    // 标注快照：每帧覆盖写一份与 ImGui 无关的副本，供 PNG 导出栅格化
    //（导出可能发生在下一帧的模态框回调里，所以必须每帧刷新）
    publishOverlaySnapshot();
}

// -----------------------------------------------------------------------------
// 标题栏
// -----------------------------------------------------------------------------
void Workbench::renderTitleBar(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    beginRegion("##region_title", pos, size, p.panel, 0.0f, false);
    ImGui::SetCursorPos(ImVec2(14, 0));
    ImGui::SetCursorPosY(size.y * 0.5f - ImGui::GetFontSize() * 0.5f);
    // 应用标识：透镜 + 分子（与 exe 图标同源，矢量绘制，任意 DPI 清晰）
    const ImVec2 markPos = ImGui::GetCursorScreenPos();
    aurora::logoMark(ImGui::GetWindowDrawList(),
                     ImVec2(markPos.x + 10.0f, markPos.y + ImGui::GetFontSize() * 0.5f),
                     21.0f, col(p.accent), col(p.accent));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28.0f);
    ImGui::SetCursorPosY(size.y * 0.5f - ImGui::GetFontSize() * 0.5f);
    ImGui::TextColored(p.text, "MolecuLens");
    ImGui::SameLine();
    ImGui::TextColored(p.textFaint, "v2 · Aurora");
    ImGui::SameLine(0, 16);
    ImGui::TextColored(p.borderStrong, "|");
    ImGui::SameLine(0, 16);
    ImGui::TextColored(p.textMuted, "%s", documentName().c_str());

    // 拖动区（右侧按钮之前）
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 420.0f, pos.y));
    ImGui::InvisibleButton("##title_drag", ImVec2(std::max(1.0f, size.x - 420.0f - 140.0f),
                                                  size.y));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0) && svc_.dragWindow) {
        svc_.dragWindow(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && svc_.toggleMaximize) {
        svc_.toggleMaximize();
    }

    // 窗口按钮
    struct Btn { const char* id; chrome::WindowGlyph glyph; };
    const bool maximized = svc_.isMaximized ? svc_.isMaximized() : false;
    const Btn buttons[3] = {
        {"##min", chrome::WindowGlyph::Minimize},
        {"##max", maximized ? chrome::WindowGlyph::Restore : chrome::WindowGlyph::Maximize},
        {"##close", chrome::WindowGlyph::Close},
    };
    const float bw = 40.0f;
    float bx = pos.x + size.x - bw * 3.0f - 6.0f;
    for (int i = 0; i < 3; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(bx, pos.y));
        const bool clicked = ImGui::InvisibleButton(buttons[i].id, ImVec2(bw, size.y));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(bx, pos.y), ImVec2(bx + bw, pos.y + size.y),
                col(i == 2 ? p.danger : p.accentSoft), 0.0f);
        }
        chrome::drawWindowGlyph(
            ImVec2(bx + bw * 0.5f - 7.0f, pos.y + size.y * 0.5f - 7.0f), ImVec2(14, 14),
            buttons[i].glyph);
        if (clicked) {
            if (i == 0 && svc_.minimizeWindow) {
                svc_.minimizeWindow();
            } else if (i == 1 && svc_.toggleMaximize) {
                svc_.toggleMaximize();
            } else if (i == 2 && svc_.requestClose) {
                svc_.requestClose();
            }
        }
        bx += bw;
    }
    endRegion();
}

// -----------------------------------------------------------------------------
// 菜单栏
// -----------------------------------------------------------------------------
void Workbench::renderMenuBar(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    beginRegion("##region_menu", pos, size, p.panel, 0.0f, false, ImGuiWindowFlags_MenuBar);
    ImGui::SetCursorPos(ImVec2(10, 0));
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(tr("文件"))) {
            if (ImGui::MenuItem(tr("打开..."), "Ctrl+O")) {
                openFileDialog();
            }
            if (ImGui::MenuItem(tr("保存场景..."), "Ctrl+S")) {
                saveSceneDialog();
            }
            if (ImGui::MenuItem(tr("导入轨迹 (.dcd/.xtc)..."))) {
                importTrajectoryDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("导出图像..."), "Ctrl+E")) {
                exportPngDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("退出"), "Alt+F4") && svc_.requestClose) {
                svc_.requestClose();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("视图"))) {
            if (ImGui::MenuItem(tr("取景全部"), "F")) {
                frameCamera();
            }
            if (ImGui::MenuItem(tr("重置视角"), "R")) {
                resetCamera();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("浅色 / 深色"), nullptr, aurora::isDark())) {
                toggleTheme();
            }
            if (ImGui::MenuItem(railCollapsed_ ? tr("展开工作区") : tr("收起工作区"),
                                "Ctrl+B", railCollapsed_)) {
                toggleRailCollapsed();
            }
            ImGui::Separator();
            for (const auto& m : modules_) {
                if (ImGui::MenuItem(tr(m->label()), nullptr,
                                    activeId_ == m->id())) {
                    setModule(m->id());
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("样式"))) {
            if (ImGui::MenuItem(tr("sob-art"))) applyPreset(kSobArt);
            if (ImGui::MenuItem(tr("IBOview"))) applyPreset(kIBOview);
            if (ImGui::MenuItem(tr("HoukMol"))) applyPreset(kHoukMol);
            if (ImGui::MenuItem(tr("IQmol"))) applyPreset(kIQmol);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("出版风格（黑底平光）"))) applyPublicationStyle();
            if (ImGui::MenuItem(tr("清空样式"))) applyPreset(kClearStyle);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("帮助"))) {
            if (ImGui::MenuItem(tr("快捷键与关于"), "F1")) {
                showHelp_ = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    endRegion();
}

// -----------------------------------------------------------------------------
// 文件栏
// -----------------------------------------------------------------------------
void Workbench::renderFileBar(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    beginRegion("##region_filebar", pos, size, p.panel, 0.0f, false);
    ImGui::SetCursorPos(ImVec2(12, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    if (ImGui::Button(tr("打开文件"), ImVec2(92, 28))) {
        openFileDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("保存场景"), ImVec2(92, 28))) {
        saveSceneDialog();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(220.0f, size.x - 560.0f));
    const std::string path = hasDocument() ? svc_.ioCtl->currentSourcePath()
                                           : std::string(tr("尚未打开文件 · 支持 PDB / GRO / SDF / CUBE"));
    ImGui::PushStyleColor(ImGuiCol_Text, hasDocument() ? p.text : p.textFaint);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.panelSunken);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", path.c_str());
    ImGui::InputText("##path", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor(2);

    ImGui::SameLine();
    if (ImGui::Button(tr("导出 PNG"), ImVec2(92, 28))) {
        exportPngDialog();
    }
    ImGui::SameLine();
    // 语言切换：保留"以中文源串为键"的 i18n 约定，只切换全局语言。
    const bool en = i18n::current() == i18n::Language::En;
    if (ImGui::Button(en ? "EN" : "中", ImVec2(40, 28))) {
        i18n::setLanguage(en ? i18n::Language::Zh : i18n::Language::En);
        Settings s = loadSettings();
        s.language = i18n::languageKey(i18n::current());
        SettingsStore::save(s);
    }
    ImGui::SameLine();
    if (ImGui::Button(aurora::isDark() ? tr("浅色") : tr("深色"), ImVec2(52, 28))) {
        toggleTheme();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("设置"), ImVec2(52, 28))) {
        setModule("settings");
    }
    ImGui::PopStyleVar();
    endRegion();
}

// -----------------------------------------------------------------------------
// 左导航
// -----------------------------------------------------------------------------
void Workbench::renderRail(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    beginRegion("##region_rail", pos, size, p.panelAlt, 12.0f, true);

    // —— 顶部：收起 / 展开开关（« / »），点一下把工作区收到只剩图标 ——
    const float toggleW = 26.0f;
    ImGui::SetCursorPos(ImVec2((size.x - toggleW) * 0.5f, 7.0f));
    const ImVec2 tpos = ImGui::GetCursorScreenPos();
    const bool toggleClicked = ImGui::InvisibleButton("##rail_toggle", ImVec2(toggleW, 22.0f));
    {
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov) {
            dl->AddRectFilled(tpos, ImVec2(tpos.x + toggleW, tpos.y + 22.0f),
                              col(p.accentSoft), 7.0f);
        }
        const ImU32 c = col(hov ? p.accent : p.textFaint);
        const ImVec2 m(tpos.x + toggleW * 0.5f, tpos.y + 11.0f);
        // 展开态画 «（点它收起），收起态画 »（点它展开）。
        const float dir = railCollapsed_ ? 1.0f : -1.0f;
        for (int k = 0; k < 2; ++k) {
            const float ox = m.x + dir * (-2.6f + static_cast<float>(k) * 3.4f);
            dl->AddLine(ImVec2(ox + dir * 3.0f, m.y - 4.5f), ImVec2(ox, m.y), c, 1.6f);
            dl->AddLine(ImVec2(ox, m.y), ImVec2(ox + dir * 3.0f, m.y + 4.5f), c, 1.6f);
        }
        if (hov) {
            ImGui::SetTooltip("%s (Ctrl+B)",
                              railCollapsed_ ? tr("展开工作区") : tr("收起工作区"));
        }
    }
    if (toggleClicked) {
        toggleRailCollapsed();
    }

    if (!railCollapsed_) {
        ImGui::SetCursorPos(ImVec2(12, 33));
        ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
        ImGui::TextUnformatted(tr("工作区"));
        ImGui::PopStyleColor();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
    }

    const float topOffset = railCollapsed_ ? 40.0f : 46.0f;
    for (size_t i = 0; i < modules_.size(); ++i) {
        const auto& m = modules_[i];
        const float btnX = railCollapsed_ ? 8.0f : 8.0f;
        ImGui::SetCursorPosX(btnX);
        // 导航项高度自适应：窗口矮（1280×800 或高 DPI 缩放）时压缩按钮，
        // 保证 9 个工作区全部可见，而不是被裁掉最后两个。
        const float count = static_cast<float>(std::max<size_t>(1, modules_.size()));
        const float btnH = std::clamp((size.y - topOffset) / count - 3.0f, 26.0f,
                                      aurora::metrics::kRailButtonH - 6.0f);
        const bool active = activeId_ == m->id();
        if (aurora::navButton(railCollapsed_ ? "" : tr(m->label()), active,
                              ImVec2(size.x - 16.0f, btnH), static_cast<int>(i))) {
            setModule(m->id());
        }
        if (railCollapsed_ && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr(m->label()));
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
    }

    if (!railCollapsed_) {
        ImGui::SetCursorPos(ImVec2(12, size.y - 22.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
        ImGui::TextUnformatted("v2.0");
        ImGui::PopStyleColor();
    }
    endRegion();
}

// -----------------------------------------------------------------------------
// 中央视口
// -----------------------------------------------------------------------------
void Workbench::renderViewport(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    viewportMin_ = pos;
    viewportMax_ = add(pos, size);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panelSunken);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##viewport", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 视口底：柔和渐变。空场景时它就是"画布"，有分子时 FBO 会整块覆盖。
    dl->AddRectFilledMultiColor(pos, add(pos, size), col(p.gradTop), col(p.gradTop),
                                col(p.gradBottom), col(p.gradBottom));

    // 分子画面矩形（在 render() 之后立即取；相机交互与标注都要用）。
    // 必须在 if/else 之前声明：时间轴子窗口渲染后 GetItemRect() 会变成时间轴
    // 里的最后一个控件，若那时才取，导入轨迹后相机输入区整体错位。
    ImVec2 imgMin(0.0f, 0.0f);
    ImVec2 imgMax(0.0f, 0.0f);
    const bool empty = totalMolecules() == 0;
    if (empty) {
        const char* title = tr("打开一个结构文件开始");
        const char* sub = tr("支持 PDB / GRO / SDF / CUBE；也可直接拖入窗口");
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y * 0.42f));
        textCentered(title, p.text);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y * 0.42f + 28.0f));
        textCentered(sub, p.textMuted);
        ImGui::SetCursorScreenPos(
            ImVec2(pos.x + size.x * 0.5f - 70.0f, pos.y + size.y * 0.42f + 62.0f));
        if (aurora::chip(tr("打开文件"), true)) {
            openFileDialog();
        }
    } else if (svc_.viewport != nullptr) {
        // 满幅画面（v2.8）：分子场景铺满整张卡片，左右不再留白；
        // 导入轨迹后，画面下方单独让出一条时间轴（TransportBar + Scrubber）。
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool hasTraj = svc_.timelineCtl != nullptr &&
                             svc_.timelineCtl->frameCount() > 1;
        const float stripH = hasTraj ? 74.0f : 0.0f;
        const ImVec2 canvas(std::max(avail.x, 64.0f),
                            std::max(avail.y - stripH, 64.0f));
        // 视图叠加开关（坐标轴 / 比例尺+尺寸）与播放期低细节网格
        svc_.viewport->setViewOverlay(showViewAxes_, showViewScale_);
        if (svc_.timelineCtl != nullptr) {
            svc_.viewport->setTrajectoryFrame(svc_.timelineCtl->frame());
        }
        svc_.viewport->render(canvas);
        // ⚠ 必须在 render() 之后、时间轴等其它控件之前立刻取图像矩形：
        // 视口下方的 InvisibleButton（相机交互）依赖它。此前该矩形在时间轴
        // 子窗口渲染之后才取，GetItemRect() 返回的是时间轴里的最后一个控件，
        // 于是**导入轨迹后**相机输入区整体错位 —— 视口里拖不动（旋转）、
        // 滚轮也不缩放。这是一个只随"有无时间轴"出现的隐蔽回归。
        imgMin = ImGui::GetItemRectMin();
        imgMax = ImGui::GetItemRectMax();
        // 分子名标注（v2.9）：每个结构的质心投影到屏幕，画成浮动标签。
        // 坐标来源 coordsSoA 按实例顺序分段（段长 = 结构原子数），用 globalAtomBase 取偏移。
        if (showMolLabels_ && svc_.scene != nullptr && svc_.data != nullptr &&
            svc_.viewport != nullptr) {
            const engine::OrbitCamera* lbCam = svc_.viewport->getCamera();
            if (lbCam != nullptr) {

                const float lbAspect = canvas.y > 1.0f ? canvas.x / canvas.y : 1.0f;
                const glm::mat4 lbVp = lbCam->viewProjMatrix(lbAspect);
                const auto& lbInsts = svc_.scene->instances();
                const auto& lbPool = svc_.scene->structures();
                const auto& lbCoords = svc_.scene->coordsSoA().positions;
                ImDrawList* lbDl = ImGui::GetWindowDrawList();
                std::unordered_set<uint32_t> lbSeen;
                std::vector<ImVec4> lbPlaced;
                for (size_t li = 0; li < lbInsts.size(); ++li) {
                    const auto& inst = lbInsts[li];
                    if (inst.structureIndex >= lbPool.size() || !inst.visible ||
                        !lbSeen.insert(inst.structureIndex).second) {
                        continue;
                    }
                    const size_t lbN = lbPool[inst.structureIndex].elements.size();
                    const uint32_t lbBase = svc_.scene->globalAtomBase(static_cast<uint32_t>(li));
                    if (lbN == 0 || static_cast<size_t>(lbBase) + lbN > lbCoords.size()) {
                        continue;
                    }
                    glm::vec3 lbC(0.0f);
                    for (size_t k = 0; k < lbN; ++k) {
                        lbC += lbCoords[lbBase + k];
                    }
                    lbC /= static_cast<float>(lbN);
                    float lbPx = 0.0f;
                    float lbPy = 0.0f;
                    if (!engine::esp_overlay::projectToPixel(lbVp, lbC, static_cast<int>(canvas.x),
                                                static_cast<int>(canvas.y), lbPx, lbPy)) {
                        continue;
                    }
                    std::string lbName = svc_.data->moleculeName(li);
                    if (lbName.size() > 18) {
                        lbName = lbName.substr(0, 17) + "…";
                    }
                    if (lbName.empty()) {
                        continue;
                    }
                    const ImVec2 lbTsz = ImGui::CalcTextSize(lbName.c_str());
                    ImVec2 lbTp(pos.x + lbPx + 8.0f, pos.y + lbPy - 9.0f);
                    // 防重叠：与已放置标签相交就向下错开 16px（最多 6 次）。
                    for (int guard = 0; guard < 6; ++guard) {
                        bool lbHit = false;
                        for (const ImVec4& lbR : lbPlaced) {
                            if (lbTp.x < lbR.z && lbTp.x + lbTsz.x + 10.0f > lbR.x &&
                                lbTp.y < lbR.w && lbTp.y + lbTsz.y + 4.0f > lbR.y) {
                                lbHit = true;
                                break;
                            }
                        }
                        if (!lbHit) {
                            break;
                        }
                        lbTp.y += 16.0f;
                    }
                    lbPlaced.push_back(ImVec4(lbTp.x, lbTp.y, lbTp.x + lbTsz.x + 10.0f,
                                              lbTp.y + lbTsz.y + 4.0f));
                    lbDl->AddRectFilled(ImVec2(lbTp.x - 5.0f, lbTp.y - 2.0f),
                                        ImVec2(lbTp.x + lbTsz.x + 5.0f, lbTp.y + lbTsz.y + 2.0f),
                                        IM_COL32(16, 20, 28, 165), 3.0f);
                    lbDl->AddRect(ImVec2(lbTp.x - 5.0f, lbTp.y - 2.0f),
                                  ImVec2(lbTp.x + lbTsz.x + 5.0f, lbTp.y + lbTsz.y + 2.0f),
                                  IM_COL32(120, 160, 220, 110), 3.0f);
                    lbDl->AddText(lbTp, IM_COL32(245, 248, 252, 245), lbName.c_str());
                }
            }
        }

        if (hasTraj && timeAxis_ != nullptr) {
            // 用视口渲染后**实际剩余**的高度给时间轴，避免被卡片底边裁掉
            // （固定 stripH 时，视口渲染的边距会把第二行拖动条挤出可视区）。
            const float y0 = ImGui::GetCursorPosY();
            const float remain = ImGui::GetContentRegionAvail().y;
            const float h = std::min(stripH, std::max(remain - 2.0f, 34.0f));
            ImGui::SetCursorPos(ImVec2(0.0f, y0));
            ImGui::BeginChild("##timeline_strip", ImVec2(canvas.x, h), true,
                              ImGuiWindowFlags_NoScrollbar);
            timeAxis_->setController(svc_.timelineCtl);
            timeAxis_->render();
            ImGui::EndChild();
        }
    }

    // 相机交互：整块**分子画面**一个隐形按钮（imgMin/imgMax 在上面紧接着
    // render() 取好，不含时间轴条）；悬浮工具条区域除外。
    if (!empty) {
        ImGui::SetCursorScreenPos(imgMin);
        ImGui::InvisibleButton("##cam_input",
                               ImVec2(imgMax.x - imgMin.x, imgMax.y - imgMin.y),
                               ImGuiButtonFlags_MouseButtonLeft |
                                   ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        // 拖到视口外时 ImGui 的 Hovered 会变假，但 Active 仍为真：必须用
        // Active 续拖，否则快速拖动时旋转/平移会中途"断电"。
        const bool active = ImGui::IsItemActive();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        bool overOverlay = false;
        for (const ImVec4& r : vpOverlayRects_) {
            if (mouse.x >= r.x && mouse.x <= r.x + r.z && mouse.y >= r.y &&
                mouse.y <= r.y + r.w) {
                overOverlay = true;
                break;
            }
        }
        if ((hovered || active) && !overOverlay && svc_.viewport != nullptr) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            for (int b = 0; b < 3; ++b) {
                if (ImGui::IsMouseDragging(b, 0.0f) || ImGui::IsMouseDown(b)) {
                    svc_.viewport->handleMouse(d.x, d.y, b);
                }
            }
            const float wheel = ImGui::GetIO().MouseWheel;
            if (hovered && wheel != 0.0f) {
                svc_.viewport->handleScroll(wheel);
            }
        }
        // 场景标注（盒子 / 箭头 / 文字标签）：画在 3D 图之上、悬浮按钮之下
        drawSceneOverlay(imgMin, imgMax);
    }

    // —— 悬浮工具条（左上）——
    vpOverlayRects_.clear();
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0f, pos.y + 12.0f));
    if (aurora::toolButton(tr("取景"), tr("让全部原子充满视口（F）"))) {
        frameCamera();
    }
    ImGui::SameLine(0, 6);
    if (aurora::toolButton(tr("重置"), tr("恢复默认相机（R）"))) {
        resetCamera();
    }
    ImGui::SameLine(0, 6);
    if (aurora::toolButton(tr("截图"), tr("导出当前视口的 PNG（Ctrl+E）"))) {
        exportPngDialog();
    }
    ImGui::SameLine(0, 6);
    const bool rt = svc_.viewport != nullptr && svc_.viewport->rayTracePreview();
    if (aurora::toolButton(rt ? tr("光追 · 开") : tr("光追 · 关"),
                           tr("用 CPU 光线追踪实时预览（较慢，质量更高）"))) {
        if (svc_.viewport != nullptr) {
            svc_.viewport->setRayTracePreview(!rt);
            notify(rt ? "已关闭光追预览" : "已开启光追预览（低分辨率渐进）");
        }
    }
    ImGui::SameLine(0, 6);
    if (aurora::toolButton(tr("导出…"), tr("按倍数导出高分辨率图片"))) {
        exportPngDialog();
    }
    ImGui::SameLine(0, 6);
    const bool anyOverlay = showViewAxes_ || showViewScale_;
    if (aurora::toolButton(anyOverlay ? tr("叠加 · 开") : tr("叠加 · 关"),
                           tr("坐标轴方向 / 比例尺 / 场景尺寸（可分别关闭）"))) {
        ImGui::OpenPopup("##vp_overlay_menu");
    }
    if (ImGui::BeginPopup("##vp_overlay_menu")) {
        ImGui::TextDisabled("%s", tr("分子显示视图"));
        ImGui::Checkbox(tr("坐标轴方向 (X/Y/Z)"), &showViewAxes_);
        ImGui::Checkbox(tr("比例尺与尺寸 (Å)"), &showViewScale_);
        ImGui::Separator();
        if (ImGui::MenuItem(tr("全部关闭"))) {
            showViewAxes_ = false;
            showViewScale_ = false;
        }
        if (ImGui::MenuItem(tr("全部打开"))) {
            showViewAxes_ = true;
            showViewScale_ = true;
        }
        ImGui::EndPopup();
    }
    vpOverlayRects_.push_back(ImVec4(pos.x + 8.0f, pos.y + 8.0f, 400.0f, 34.0f));

    // 时间轴条占位：底部悬浮状态与 ESP 色标必须上移，否则会压在时间轴上
    //（用户反馈"有时间轴后 esp 轴被挡"）。74 = renderViewport 里的 stripH。
    const bool hasTimelineNow = svc_.timelineCtl != nullptr &&
                                svc_.timelineCtl->frameCount() > 1;
    const float bottomReserve = hasTimelineNow ? 74.0f : 0.0f;

    // —— 左下状态 ——
    float infoY = pos.y + size.y - 30.0f - bottomReserve;
    if (svc_.renderer != nullptr && svc_.renderer->surfaceGenerationPending()) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0f, infoY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, p.accentSoft);
        ImGui::PushStyleColor(ImGuiCol_Text, p.accent);
        ImGui::Button(tr("表面生成中…"), ImVec2(120, 24));
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        infoY -= 30.0f;
    }
    if (hideHydrogen_) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0f, infoY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, p.panelAlt);
        ImGui::PushStyleColor(ImGuiCol_Text, p.textMuted);
        ImGui::Button(tr("已隐藏 H"), ImVec2(84, 22));
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    // —— 右下 ESP 色标 ——
    engine::Representation* rep = activeRep();
    if (rep != nullptr && rep->type == engine::RepresentationType::EspSurface &&
        rep->visible) {
        const float barW = std::min(240.0f, size.x * 0.42f);
        const ImVec2 b0(pos.x + size.x - barW - 16.0f,
                        pos.y + size.y - 46.0f - bottomReserve);
        const ImVec2 b1(b0.x + barW, b0.y + 12.0f);
        ImDrawList* fg = ImGui::GetWindowDrawList();
        const auto cmap = static_cast<engine::EspColormap>(std::clamp<int32_t>(
            rep->espColormap, 0, static_cast<int32_t>(engine::EspColormap::Count) - 1));
        const float vmin = rep->espVminKcal;
        const float vmax = rep->espVmaxKcal;
        const int steps = 40;
        for (int i = 0; i < steps; ++i) {
            const float t0 = static_cast<float>(i) / steps;
            const float t1 = static_cast<float>(i + 1) / steps;
            const glm::vec3 c = engine::espColormapColor(cmap, (t0 + t1) * 0.5f);
            const ImU32 c32 = IM_COL32(static_cast<int>(c.r * 255),
                                       static_cast<int>(c.g * 255),
                                       static_cast<int>(c.b * 255), 255);
            fg->AddRectFilled(
                ImVec2(b0.x + barW * t0, b0.y),
                ImVec2(b0.x + barW * t1 + 1.0f, b1.y), c32);
        }
        fg->AddRect(b0, b1, col(p.border), 3.0f);
        char lo[32], hi[32];
        std::snprintf(lo, sizeof(lo), "%.1f", vmin);
        std::snprintf(hi, sizeof(hi), "%.1f", vmax);
        fg->AddText(ImVec2(b0.x, b1.y + 3.0f), col(p.textMuted), lo);
        const ImVec2 hs = ImGui::CalcTextSize(hi);
        fg->AddText(ImVec2(b1.x - hs.x, b1.y + 3.0f), col(p.textMuted), hi);
        fg->AddText(ImVec2(b0.x + barW * 0.5f - 26.0f, b0.y - 16.0f), col(p.textMuted),
                    "kcal/mol");
        vpOverlayRects_.push_back(ImVec4(b0.x - 4.0f, b0.y - 20.0f, barW + 8.0f, 34.0f));
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// -----------------------------------------------------------------------------
// 右侧检查器
// -----------------------------------------------------------------------------
void Workbench::renderInspector(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();

    // 宽度拖拽手柄（属于根窗口，必须在 BeginChild 之前）
    ImGui::SetCursorScreenPos(ImVec2(pos.x - 5.0f, pos.y));
    ImGui::InvisibleButton("##insp_resize", ImVec2(10.0f, size.y));
    inspectorSplitHot_ = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (inspectorSplitHot_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        // v2.8：悬停/拖动时亮一条主色细线，告诉用户这里可以拖
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(pos.x - 1.0f, pos.y + 12.0f),
            ImVec2(pos.x + 1.0f, pos.y + size.y - 12.0f), col(p.accent), 1.0f);
    }
    if (ImGui::IsItemActive()) {
        inspectorW_ = std::clamp(inspectorW_ - ImGui::GetIO().MouseDelta.x,
                                 aurora::metrics::kInspectorMin,
                                 std::min(aurora::metrics::kInspectorMax,
                                          ImGui::GetMainViewport()->Size.x * 0.62f));
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##inspector", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    IInspectorModule* mod = findModule(activeId_.c_str());
    if (mod != nullptr) {
        ImGui::PushFont(UITheme::fontTitle() != nullptr ? UITheme::fontTitle()
                                                        : ImGui::GetFont());
        ImGui::TextColored(p.text, "%s", tr(mod->label()));
        ImGui::PopFont();
        if (mod->subtitle() != nullptr && mod->subtitle()[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, p.textMuted);
            ImGui::TextWrapped("%s", tr(mod->subtitle()));
            ImGui::PopStyleColor();
        }
        aurora::spacer(2.0f);
        aurora::hline();
        aurora::spacer(2.0f);

        ImGui::BeginChild("##insp_scroll", ImVec2(0, 0), 0,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        mod->render(*this);
        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// -----------------------------------------------------------------------------
// 底部快捷条
// -----------------------------------------------------------------------------
void Workbench::renderQuickBar(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    beginRegion("##region_quickbar", pos, size, p.panel, 10.0f, true);
    ImGui::SetCursorPos(ImVec2(10, 8));

    // 样式组：只保留 sob-art 与清空。IBOview / HoukMol / IQmol 已从快捷条移除
    //（用户反馈这三项占位意义不大），仍可在「样式」菜单里使用。
    if (aurora::chip("sob-art", preset_ == kSobArt, "球棍 + CPK + 深背景（论文插图常用）")) {
        applyPreset(kSobArt);
    }
    ImGui::SameLine(0, 6);
    if (aurora::chip(tr("清空样式"), false, "删除全部表示，恢复默认球棍")) {
        applyPreset(kClearStyle);
    }
    ImGui::SameLine(0, 12);
    ImGui::TextColored(p.border, "|");
    ImGui::SameLine(0, 12);
    if (aurora::chip(tr("隐藏氢"), hideHydrogen_,
                     "把表示的 selection 改成 not element H；再次点击恢复")) {
        setHydrogenHidden(!hideHydrogen_);
    }
    ImGui::SameLine(0, 6);
    if (aurora::chip(tr("分子名"), showMolLabels_,
                     "在画面上标出每个分子/结构名称（投影到屏幕的浮动标签）")) {
        showMolLabels_ = !showMolLabels_;
    }    ImGui::SameLine(0, 6);
    // 网格质量：直接改当前表面/快速表面的间距（原先要翻到「表面」页拖滑杆）
    if (aurora::chip(tr("网格质量"), false, "当前表面 / 快速表面的网格间距")) {
        ImGui::OpenPopup("##grid_popup");
    }
    if (ImGui::BeginPopup("##grid_popup")) {
        ImGui::TextUnformatted(tr("网格间距"));
        engine::Representation* ar = activeRep();
        const bool isSurface =
            ar != nullptr && (ar->type == engine::RepresentationType::Surface ||
                              ar->type == engine::RepresentationType::QuickSurf);
        if (!isSurface) {
            ImGui::TextColored(p.textMuted, "%s", tr("当前表示不是表面 / 快速表面"));
        } else {
            struct GridLevel {
                const char* label;
                float spacing;
            };
            const GridLevel levels[4] = {{tr("粗 1.0 Å"), 1.00f},
                                         {tr("中 0.5 Å"), 0.50f},
                                         {tr("细 0.25 Å"), 0.25f},
                                         {tr("极细 0.15 Å"), 0.15f}};
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(i);
                const bool on = std::abs(ar->surfGridSpacing - levels[i].spacing) < 0.01f;
                if (aurora::chip(levels[i].label, on, "")) {
                    if (svc_.repCtl != nullptr) {
                        svc_.repCtl->setSurfGridSpacing(activeRepIndex(), levels[i].spacing);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
                if (i < 3) {
                    ImGui::SameLine(0, 6);
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 6);
    if (aurora::chip(tr("背景"), false, "背景色：预设色板 + HSV 取色器")) {
        ImGui::OpenPopup("##bg_popup");
    }
    if (ImGui::BeginPopup("##bg_popup")) {
        float bg[3] = {0.10f, 0.10f, 0.15f};
        if (svc_.scene != nullptr) {
            const auto& props = svc_.scene->properties();
            bg[0] = props.bgColor[0];
            bg[1] = props.bgColor[1];
            bg[2] = props.bgColor[2];
        }
        ImGui::TextUnformatted(tr("背景色"));
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::ColorEdit3("##quick_bg", bg,
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_PickerHueWheel)) {
            if (svc_.renderCtl != nullptr) {
                svc_.renderCtl->setBackgroundColor(bg[0], bg[1], bg[2]);
            }
        }
        ImGui::Spacing();
        struct BgSwatch {
            const char* name;
            ImVec4 c;
        };
        const BgSwatch swatches[5] = {
            {"深灰", ImVec4(0.22f, 0.24f, 0.28f, 1.0f)},
            {"白色", ImVec4(1.00f, 1.00f, 1.00f, 1.0f)},
            {"浅灰", ImVec4(0.93f, 0.95f, 0.97f, 1.0f)},
            {"深蓝", ImVec4(0.06f, 0.08f, 0.12f, 1.0f)},
            {"黑色", ImVec4(0.00f, 0.00f, 0.00f, 1.0f)},
        };
        for (int i = 0; i < 5; ++i) {
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Border, aurora::palette().borderStrong);
            const bool clicked = ImGui::ColorButton(
                "##bgsw", swatches[i].c, ImGuiColorEditFlags_NoTooltip,
                ImVec2(30, 30));
            ImGui::PopStyleColor();
            if (clicked) {
                if (svc_.renderCtl != nullptr) {
                    svc_.renderCtl->setBackgroundColor(swatches[i].c.x,
                                                       swatches[i].c.y,
                                                       swatches[i].c.z);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", swatches[i].name);
            }
            ImGui::PopID();
            if (i < 4) {
                ImGui::SameLine(0, 6);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 6);
    {
        const bool rt = svc_.viewport != nullptr && svc_.viewport->rayTracePreview();
        if (aurora::chip(tr("光追预览"), rt, "切换光线追踪预览（较慢，成图前检查用）")) {
            if (svc_.viewport != nullptr) {
                svc_.viewport->setRayTracePreview(!rt);
            }
        }
    }
    ImGui::SameLine(0, 6);
    if (aurora::chip(tr("导出 PNG"), false, "导出当前视口")) {
        exportPngDialog();
    }

    // 右端状态（v2.7）：指标胶囊，表示数 + 原子数
    char reps[32];
    std::snprintf(reps, sizeof(reps), "%d", static_cast<int>(representations().size()));
    char atoms[32] = "-";
    if (svc_.scene != nullptr) {
        std::snprintf(atoms, sizeof(atoms), "%d",
                      static_cast<int>(svc_.scene->coordsSoA().positions.size()));
    }
    const float chipRepsW = ImGui::CalcTextSize(tr("表示")).x + ImGui::CalcTextSize(reps).x + 24.0f;
    const float chipAtomsW = ImGui::CalcTextSize(tr("原子")).x + ImGui::CalcTextSize(atoms).x + 24.0f;
    ImGui::SameLine(size.x - chipRepsW - chipAtomsW - 20.0f);
    aurora::statChip(tr("原子"), atoms);
    ImGui::SameLine(0.0f, 6.0f);
    aurora::statChip(tr("表示"), reps);
    endRegion();
}

// -----------------------------------------------------------------------------
// 状态栏
// -----------------------------------------------------------------------------
void Workbench::renderStatusBar(const ImVec2& pos, const ImVec2& size) {
    const Palette& p = palette();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textMuted);
    ImGui::PushFont(UITheme::fontSmall() != nullptr ? UITheme::fontSmall()
                                                    : ImGui::GetFont());

    char left[256];
    std::snprintf(left, sizeof(left), "%.0f FPS   ·   %d 原子   ·   %d 分子   ·   %s",
                  fps(), totalAtoms(), totalMolecules(),
                  hasDocument() ? tr("已加载") : tr("空场景"));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 6.0f, pos.y + 5.0f));
    ImGui::TextUnformatted(left);

    char frameTxt[96] = "";
    if (svc_.timelineCtl != nullptr && svc_.timelineCtl->frameCount() > 0) {
        std::snprintf(frameTxt, sizeof(frameTxt), tr("帧 %llu / %llu"),
                      static_cast<unsigned long long>(svc_.timelineCtl->frame() + 1),
                      static_cast<unsigned long long>(svc_.timelineCtl->frameCount()));
        ImGui::SameLine(0, 18);
        ImGui::TextUnformatted(frameTxt);
    }

    const char* backend = "Vulkan · MSAA";
    const float bw = ImGui::CalcTextSize(backend).x;
    ImGui::SameLine(size.x - bw - 12.0f);
    ImGui::TextUnformatted(backend);
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

// -----------------------------------------------------------------------------
// 提示气泡
// -----------------------------------------------------------------------------
void Workbench::renderToast() {
    if (toast_.empty() || toastAge_ > 3.2f) {
        return;
    }
    const float alpha = toastAge_ < 2.6f ? 1.0f : (3.2f - toastAge_) / 0.6f;
    const ImVec2 center((viewportMin_.x + viewportMax_.x) * 0.5f,
                        viewportMin_.y + 26.0f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.94f * alpha);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette().panel);
    ImGui::PushStyleColor(ImGuiCol_Border, palette().border);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushStyleColor(ImGuiCol_Text, palette().text);
    ImGui::TextUnformatted(toast_.c_str());
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleColor(2);
}

// -----------------------------------------------------------------------------
// 模态框：导出 / 帮助
// -----------------------------------------------------------------------------
void Workbench::renderModals() {
    const Palette& p = palette();
    renderSaveSceneModal();
    if (showExport_) {
        ImGui::OpenPopup(tr("导出图片###export_modal"));
        showExport_ = false;
    }
    ImGui::SetNextWindowPos(
        ImVec2((viewportMin_.x + viewportMax_.x) * 0.5f,
               (viewportMin_.y + viewportMax_.y) * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(tr("导出图片###export_modal"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const uint32_t vw = svc_.viewport != nullptr ? svc_.viewport->displayWidth() : 0;
        const uint32_t vh = svc_.viewport != nullptr ? svc_.viewport->displayHeight() : 0;
        ImGui::TextColored(p.textMuted, tr("视口当前尺寸 %u × %u"), vw, vh);
        aurora::spacer(6.0f);
        aurora::sectionLabel(tr("超采样倍数"));
        int scale = exportScale_;
        if (ImGui::RadioButton("1×", &scale, 1)) exportScale_ = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("2×", &scale, 2)) exportScale_ = 2;
        ImGui::SameLine();
        if (ImGui::RadioButton("4×", &scale, 4)) exportScale_ = 4;
        ImGui::Checkbox(tr("透明背景"), &exportTransparent_);
        aurora::spacer(4.0f);
        ImGui::TextColored(p.textMuted, tr("输出：%u × %u 像素"),
                           vw * static_cast<uint32_t>(std::max(1, exportScale_)),
                           vh * static_cast<uint32_t>(std::max(1, exportScale_)));
        aurora::spacer(10.0f);
        if (aurora::chip(tr("导出"), true)) {
            if (svc_.dlgSave && svc_.ioCtl != nullptr) {
                const std::string filter = "PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
                const std::string path = svc_.dlgSave("moleculens.png", filter.c_str(), "png");
                if (!path.empty()) {
                    // 直接走 MolScene 的离屏导出（ioCtl->exportPng 仍是 M4 占位，
                    // 恒返回 false，用户点"导出"只会看到失败提示）。
                    const uint32_t outW =
                        vw * static_cast<uint32_t>(std::max(1, exportScale_));
                    const uint32_t outH =
                        vh * static_cast<uint32_t>(std::max(1, exportScale_));
                    const bool ok =
                        svc_.viewport != nullptr &&
                        svc_.viewport->exportToPng(outW, outH, path, nullptr, 1, 8,
                                                   exportTransparent_);
                    if (ok) {
                        notify("图片已导出：" + path);
                    } else {
                        notify("导出失败，详见日志");
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (aurora::chip(tr("取消"), false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (showHelp_) {
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(
            ImVec2((viewportMin_.x + viewportMax_.x) * 0.5f,
                   (viewportMin_.y + viewportMax_.y) * 0.5f),
            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin(tr("快捷键与关于###help"), &showHelp_,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            // 标识 + 版本（Logo 与 exe 图标同源）
            const ImVec2 aboutLogoPos = ImGui::GetCursorScreenPos();
            aurora::logoMark(ImGui::GetWindowDrawList(),
                             ImVec2(aboutLogoPos.x + 28.0f, aboutLogoPos.y + 28.0f), 56.0f,
                             col(p.accent), col(p.accent));
            ImGui::Dummy(ImVec2(56.0f, 56.0f));
            ImGui::SameLine(0, 14);
            ImGui::BeginGroup();
            ImGui::TextColored(p.text, "MolecuLens");
            ImGui::TextColored(p.textMuted, "v2 · Aurora");
            ImGui::TextColored(p.textFaint, "Vulkan · C++17 · ImGui 1.91");
            ImGui::EndGroup();
            aurora::hline();
            static const struct { const char* k; const char* v; } kKeys[] = {
                {"Ctrl+O", "打开文件"}, {"Ctrl+S", "保存场景"},
                {"Ctrl+E", "导出图片"}, {"Ctrl+L", "日志"},
                {"F", "取景全部"},     {"R", "重置视角"},
                {"Space", "播放/暂停"}, {"左/右", "单帧步进"},
                {"1", "快捷条 sob-art"}, {"H", "隐藏氢"},
                {"Ctrl+B", "收起/展开工作区"}, {"F1", "本帮助"},
            };
            if (ImGui::BeginTable("##keys", 2,
                                  ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_RowBg)) {
                for (const auto& kv : kKeys) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(p.text, "%s", kv.k);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(p.textMuted, "%s", tr(kv.v));
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
}

// -----------------------------------------------------------------------------
// 保存场景：链接式 / 嵌入式轨迹（先选模式，再弹系统另存为）
//
// 为什么放在工作台内而不是并入系统"另存为"：模式选择是**产品决策**（文件大小
// vs 可移植性），不是路径问题；系统对话框只能选路径，塞不下这个语义。
// -----------------------------------------------------------------------------
void Workbench::renderSaveSceneModal() {
    const Palette& p = palette();
    if (showSaveScene_) {
        ImGui::OpenPopup(tr("保存场景###save_scene_modal"));
        showSaveScene_ = false;
    }
    ImGui::SetNextWindowPos(
        ImVec2((viewportMin_.x + viewportMax_.x) * 0.5f,
               (viewportMin_.y + viewportMax_.y) * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(tr("保存场景###save_scene_modal"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    // —— 体积预估 ——
    // 内嵌段每条**唯一**轨迹占 frames × (atoms × 12 + 36) 字节（36 B/帧为盒子）。
    // 同一 shared_ptr<Trajectory> 被多实例共享时只内嵌一份（多实例体系常见：
    // md.gro 2042 个实例共享一条 1001 帧轨迹，复制 2042 份会到 150 GB）。
    uint64_t trajCount = 0;
    uint64_t maxFrames = 0;
    uint64_t totalAtoms = 0;
    uint64_t totalBytes = 0;
    if (svc_.scene != nullptr) {
        std::unordered_set<const entity::Trajectory*> seen;
        for (const auto& inst : svc_.scene->instances()) {
            if (!inst.trajectory || inst.trajectory->frames.empty()) {
                continue;
            }
            if (!seen.insert(inst.trajectory.get()).second) {
                continue;
            }
            const uint64_t frames = inst.trajectory->frames.size();
            const uint64_t atoms = inst.trajectory->frames.front().positions.size();
            ++trajCount;
            maxFrames = std::max(maxFrames, frames);
            totalAtoms += atoms;
            totalBytes += frames * (atoms * 12 + 36);
        }
    }

    ImGui::TextColored(p.textMuted, "%s", tr("选择轨迹的存储方式"));
    aurora::spacer(6.0f);
    aurora::hline();

    ImGui::RadioButton(tr("链接式（推荐）"), &saveSceneMode_, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textMuted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f);
    ImGui::TextWrapped("%s",
                       tr("只记录轨迹文件路径与指纹，场景文件很小；轨迹文件移动或改名后"
                           "需要重新指定。"));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    aurora::spacer(6.0f);
    ImGui::RadioButton(tr("嵌入式（单文件）"), &saveSceneMode_, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textMuted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f);
    ImGui::TextWrapped("%s",
                       tr("把轨迹帧一起写进 .mlscene，可单文件分发；体积随帧数×原子数"
                          "增长。"));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    aurora::spacer(6.0f);
    aurora::hline();
    char buf[256];
    if (trajCount == 0) {
        ImGui::TextColored(p.textMuted, "%s",
                           tr("当前场景没有轨迹，两种模式保存结果相同（只存结构与表示）。"));
    } else {
        std::snprintf(buf, sizeof(buf),
                      tr("当前场景：%llu 条轨迹 / 最多 %llu 帧 / 合计 %llu 原子"),
                      static_cast<unsigned long long>(trajCount),
                      static_cast<unsigned long long>(maxFrames),
                      static_cast<unsigned long long>(totalAtoms));
        ImGui::TextColored(p.textMuted, "%s", buf);
        std::snprintf(buf, sizeof(buf), tr("内嵌预计：%.1f MB"),
                      static_cast<double>(totalBytes) / (1024.0 * 1024.0));
        ImGui::TextColored(p.textMuted, "%s", buf);
        constexpr uint64_t kEmbeddedWarnBytes = 200ull * 1024ull * 1024ull;
        if (totalBytes >= kEmbeddedWarnBytes) {
            ImGui::TextColored(p.warn, "%s",
                               tr("体积较大（>200 MB），建议改用链接式。"));
        }
    }

    aurora::spacer(10.0f);
    if (aurora::chip(tr("保存…"), true)) {
        doSaveScene(saveSceneMode_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (aurora::chip(tr("取消"), false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace moleculens::app
