// =============================================================================
// MolecuLens v2 · app/view/container/multi_view.cpp —— 多视图实现
// 设计说明见 multi_view.h（共享文档 + 每视图独立相机/渲染器/显示筛选）。
// =============================================================================

#include "app/view/container/multi_view.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <utility>

#include <spdlog/spdlog.h>

#include "app/i18n/language.h"
#include "app/view/viewport/mol_scene.h"
#include "engine/camera/camera.h"
#include "engine/render/renderer.h"
#include "entity/scene/scene.h"

namespace moleculens::app {

namespace {

ImVec4 paneRect(float x, float y, float w, float h) { return ImVec4(x, y, w, h); }

bool hasId(const std::vector<uint32_t>& ids, uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void toggleId(std::vector<uint32_t>& ids, uint32_t id, bool on) {
    const auto it = std::find(ids.begin(), ids.end(), id);
    if (on && it == ids.end()) {
        ids.push_back(id);
    } else if (!on && it != ids.end()) {
        ids.erase(it);
    }
}

// 标题条上的紧凑按钮。返回 true = 被点击。
bool chip(const char* label, const char* tooltip) {
    const bool clicked = ImGui::SmallButton(label);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

} // namespace

MultiView::MultiView() = default;

MultiView::~MultiView() { shutdown(); }

void MultiView::init(engine::VulkanRenderer* vk, engine::Renderer* primaryRenderer,
                     entity::SceneStructure* scene, MolScene* primary,
                     engine::OrbitCamera* primaryCamera) {
    vk_ = vk;
    primaryRenderer_ = primaryRenderer;
    scene_ = scene;
    primary_ = primary;
    primaryCamera_ = primaryCamera;
    panes_.clear();
    activePane_ = 0;
    layout_ = ViewLayout::Single;
    spdlog::info("[ui] multi-view ready (layout=single)");
}

void MultiView::shutdown() {
    // 主视图的渲染器筛选必须复位：否则退出多视图后单视图/导出会继续被裁剪。
    if (primaryRenderer_ != nullptr) {
        primaryRenderer_->clearPaneFilter();
    }
    panes_.clear();
}

void MultiView::releaseImGuiTextures() {
    // 主视图纹理由 ViewControl 自己释放（它拥有 molScene_）；这里只处理副视图。
    for (auto& p : panes_) {
        if (p && p->ownScene) {
            p->ownScene->releaseImGuiTextures();
        }
    }
}

int MultiView::paneCount() const {
    switch (layout_) {
        case ViewLayout::Single:
            return 1;
        case ViewLayout::SideBySide:
        case ViewLayout::Stacked:
            return 2;
        case ViewLayout::Quad:
            return 4;
    }
    return 1;
}

void MultiView::setLayout(ViewLayout layout) {
    if (layout_ == layout) {
        return;
    }
    layout_ = layout;
    const int count = paneCount();
    if (activePane_ >= count) {
        activePane_ = count - 1;
    }
    if (layout_ == ViewLayout::Single && primaryRenderer_ != nullptr) {
        // 单视图 = 完整文档；主渲染器不能残留上一轮的筛选。
        const bool pane0Filtered =
            !panes_.empty() && panes_[0] != nullptr &&
            (!panes_[0]->allSystems || !panes_[0]->repMask.empty());
        primaryRenderer_->clearPaneFilter();
        // 主渲染器的筛选是"背着" Pane 状态清掉的：必须让 pane 0 记住的上一份
        // 掩码失效，否则切回多视图时 applyPaneFilter 会误判"没变化"而不再下发，
        // 结果视图 1 标题显示已筛选、画面里却仍在渲染被筛掉的分子。
        if (!panes_.empty() && panes_[0] != nullptr) {
            panes_[0]->lastInstMask.clear();
            panes_[0]->lastRepMask.clear();
        }
        // 之前只显示某个文件时，切回单视图要重新取景到整个场景，避免"其余
        // 分子恢复了显示但都在画面外"的错觉。
        if (pane0Filtered && primaryCamera_ != nullptr) {
            glm::vec3 center(0.0f);
            float radius = 10.0f;
            if (primaryRenderer_->sceneBounds(&center, &radius)) {
                primaryCamera_->centerOn(center, radius);
            }
        }
    }
    refitOnLayoutChange_ = true;
    if (primary_ != nullptr) {
        primary_->markDirty();
    }
}

void MultiView::setActivePane(int index) {
    activePane_ = std::clamp(index, 0, std::max(0, paneCount() - 1));
}

MultiView::Pane& MultiView::ensurePane(size_t index) {
    while (panes_.size() <= index) {
        panes_.push_back(std::make_unique<Pane>());
    }
    Pane& pane = *panes_[index];
    if (pane.molScene != nullptr) {
        return pane;
    }
    if (index == 0) {
        pane.primary = true;
        pane.molScene = primary_;
        pane.camera = primaryCamera_;
        return pane;
    }
    // 副视图：独立渲染器 + 独立相机 + 独立离屏目标。渲染器绑定同一场景，
    // 表示列表在 render() 里每帧重新绑定（控制器是表示数据的唯一持有者）。
    pane.renderer = std::make_unique<engine::Renderer>();
    pane.renderer->setScene(scene_);
    pane.renderer->setVulkanRenderer(vk_);
    // 副视图一律同步生成表面：异步表面任务是全局单队列，多视图共享时行为难以
    // 推理（完成通知会被任一渲染器消费）。副视图只在筛选/场景变更时重建。
    pane.renderer->setAsyncSurfaceEnabled(false);
    pane.ownCamera = std::make_unique<engine::OrbitCamera>();
    if (primaryCamera_ != nullptr) {
        *pane.ownCamera = *primaryCamera_;   // 初始视角与主视图一致
    }
    pane.ownScene = std::make_unique<MolScene>();
    pane.ownScene->init(pane.renderer.get(), pane.ownCamera.get(), vk_);
    pane.molScene = pane.ownScene.get();
    pane.camera = pane.ownCamera.get();
    spdlog::debug("[ui] multi-view pane {} created", index + 1);
    return pane;
}

bool MultiView::applyPaneFilter(Pane& pane,
                                const std::vector<engine::Representation>& reps,
                                bool* instanceChanged) {
    if (scene_ == nullptr) {
        return false;
    }
    // 体系列表变化（载入 .mlscene 整体替换 / 删除体系）时清理失效的勾选，
    // 避免视图永久空白（用户看不到"为什么什么都没了"）。
    if (!pane.allSystems) {
        std::vector<uint32_t> kept;
        kept.reserve(pane.systemIds.size());
        for (uint32_t id : pane.systemIds) {
            if (scene_->systemIndexById(id) != UINT32_MAX) {
                kept.push_back(id);
            }
        }
        pane.systemIds = std::move(kept);
        if (pane.systemIds.empty()) {
            pane.allSystems = true;
        }
    }

    std::vector<uint8_t> instMask;
    if (!pane.allSystems) {
        const auto& instances = scene_->instances();
        instMask.assign(instances.size(), 0u);
        for (size_t i = 0; i < instances.size(); ++i) {
            instMask[i] = hasId(pane.systemIds, instances[i].systemId) ? 1u : 0u;
        }
    }

    std::vector<uint8_t> repMask;
    if (!pane.repMask.empty()) {
        repMask = pane.repMask;
        repMask.resize(reps.size(), 1u);
        if (std::none_of(repMask.begin(), repMask.end(),
                         [](uint8_t v) { return v != 0; })) {
            repMask.clear();   // 全空 = 不让视图变成"永远空白"
        }
    }

    const bool instChanged = instMask != pane.lastInstMask;
    const bool changed = instChanged || repMask != pane.lastRepMask;
    if (!changed) {
        return false;
    }
    if (instanceChanged != nullptr) {
        *instanceChanged = instChanged;
    }
    pane.lastInstMask = instMask;
    pane.lastRepMask = repMask;
    if (pane.primary) {
        if (primaryRenderer_ != nullptr) {
            primaryRenderer_->setPaneFilter(std::move(instMask), std::move(repMask));
        }
    } else if (pane.renderer) {
        pane.renderer->setPaneFilter(std::move(instMask), std::move(repMask));
    }
    return true;
}

bool MultiView::instanceVisibleInPane(const Pane& pane, uint32_t index) const {
    if (scene_ == nullptr || index >= scene_->instances().size()) {
        return false;
    }
    const auto& inst = scene_->instances()[index];
    if (!inst.visible) {
        return false;
    }
    if (inst.systemId != UINT32_MAX) {
        const uint32_t si = scene_->systemIndexById(inst.systemId);
        if (si != UINT32_MAX && !scene_->systems()[si].visible) {
            return false;
        }
    }
    if (pane.allSystems) {
        return true;
    }
    return hasId(pane.systemIds, inst.systemId);
}

bool MultiView::paneVisibleBounds(const Pane& pane, glm::vec3* center,
                                  float* radius) const {
    if (scene_ == nullptr || center == nullptr || radius == nullptr) {
        return false;
    }
    const auto& instances = scene_->instances();
    const auto& structures = scene_->structures();
    const auto& positions = scene_->coordsSoA().positions;
    glm::vec3 lo(FLT_MAX);
    glm::vec3 hi(-FLT_MAX);
    size_t count = 0;
    for (uint32_t i = 0; i < instances.size(); ++i) {
        if (!instanceVisibleInPane(pane, i)) {
            continue;
        }
        const auto& inst = instances[i];
        if (inst.structureIndex >= structures.size()) {
            continue;
        }
        const uint32_t base = scene_->globalAtomBase(i);
        const size_t n = structures[inst.structureIndex].elements.size();
        for (size_t k = 0; k < n; ++k) {
            const size_t g = static_cast<size_t>(base) + k;
            if (g >= positions.size()) {
                break;
            }
            lo = glm::min(lo, positions[g]);
            hi = glm::max(hi, positions[g]);
            ++count;
        }
    }
    if (count == 0) {
        return false;
    }
    *center = (lo + hi) * 0.5f;
    const float r = glm::length(hi - lo) * 0.5f;
    // 下限 1.5 Å：单原子/完全重叠的体系包围球退化为点，centerOn 会把相机
    // 贴进原子里；给一个可视化下限。
    *radius = std::max(r, 1.5f);
    return true;
}

void MultiView::propagateCamera(int src) {
    if (src < 0 || static_cast<size_t>(src) >= panes_.size() ||
        panes_[static_cast<size_t>(src)] == nullptr ||
        panes_[static_cast<size_t>(src)]->camera == nullptr) {
        return;
    }
    const engine::OrbitCamera cam = *panes_[static_cast<size_t>(src)]->camera;
    for (size_t i = 0; i < panes_.size(); ++i) {
        if (static_cast<int>(i) == src || panes_[i] == nullptr ||
            panes_[i]->camera == nullptr) {
            continue;
        }
        *panes_[i]->camera = cam;
        if (panes_[i]->molScene != nullptr) {
            panes_[i]->molScene->markDirty();
        }
    }
}

void MultiView::setSyncCameras(bool on) {
    if (syncCameras_ == on) {
        return;
    }
    syncCameras_ = on;
    if (on) {
        // 开启时以主视图为准对齐一次，避免"开了同步但四处仍不一样"的困惑。
        propagateCamera(0);
    }
}

void MultiView::frameAllPanes() {
    if (syncCameras_) {
        framePane(activePane_ >= 0 ? activePane_ : 0);
        return;
    }
    for (int i = 0; i < paneCount(); ++i) {
        framePane(i);
    }
}

void MultiView::copyPrimaryCameraToAll() {
    for (int i = 0; i < paneCount(); ++i) {
        copyPrimaryCameraTo(i);
    }
}

void MultiView::framePane(int pane) {
    if (pane < 0 || static_cast<size_t>(pane) >= panes_.size() || scene_ == nullptr) {
        return;
    }
    Pane& p = *panes_[static_cast<size_t>(pane)];
    if (p.camera == nullptr) {
        return;
    }
    glm::vec3 center(0.0f);
    float radius = 40.0f;
    if (!paneVisibleBounds(p, &center, &radius)) {
        center = glm::vec3(0.0f);
        radius = 10.0f;
    }
    // 窄而高的视图：centerOn 只按竖向 FOV 计算距离，横向会被裁掉。按图像宽高比
    // 放大拟合半径（等效拉远相机），窄条/四宫格里分子才能完整入画。
    float fitRadius = radius;
    const float w = p.imageMax.x - p.imageMin.x;
    const float h = p.imageMax.y - p.imageMin.y;
    if (w > 1.0f && h > 1.0f) {
        const float aspect = w / h;
        if (aspect < 1.0f) {
            fitRadius = radius / std::max(aspect, 0.2f);
        }
    }
    p.camera->centerOn(center, fitRadius);
    if (p.molScene != nullptr) {
        p.molScene->markDirty();
    }
    if (syncCameras_) {
        propagateCamera(pane);
    }
}

void MultiView::resetPaneCamera(int pane) {
    if (pane < 0 || static_cast<size_t>(pane) >= panes_.size()) {
        return;
    }
    Pane& p = *panes_[static_cast<size_t>(pane)];
    if (p.camera == nullptr) {
        return;
    }
    p.camera->yaw = 0.0f;
    p.camera->pitch = 0.5f;
    p.camera->roll = 0.0f;
    p.camera->ortho = false;
    p.camera->fov = 45.0f;
    framePane(pane);
}

void MultiView::copyPrimaryCameraTo(int pane) {
    if (pane <= 0 || primaryCamera_ == nullptr ||
        static_cast<size_t>(pane) >= panes_.size()) {
        return;
    }
    Pane& p = *panes_[static_cast<size_t>(pane)];
    if (p.camera != nullptr) {
        *p.camera = *primaryCamera_;
        if (p.molScene != nullptr) {
            p.molScene->markDirty();
        }
        if (syncCameras_) {
            propagateCamera(pane);
        }
    }
}

bool MultiView::paneFiltered(int pane) const {
    if (pane < 0 || static_cast<size_t>(pane) >= panes_.size()) {
        return false;
    }
    const Pane& p = *panes_[static_cast<size_t>(pane)];
    return !p.allSystems || !p.repMask.empty();
}

std::string MultiView::paneFilterLabel(int pane) const {
    if (pane < 0 || static_cast<size_t>(pane) >= panes_.size() || scene_ == nullptr) {
        return {};
    }
    const Pane& p = *panes_[static_cast<size_t>(pane)];
    if (p.allSystems) {
        return {};
    }
    std::string out;
    for (uint32_t id : p.systemIds) {
        const uint32_t si = scene_->systemIndexById(id);
        if (si == UINT32_MAX) {
            continue;
        }
        if (!out.empty()) {
            out += " + ";
        }
        out += scene_->systems()[si].name;
    }
    return out;
}

size_t MultiView::systemCount() const {
    return scene_ != nullptr ? scene_->systems().size() : 0;
}

bool MultiView::paneShowsInstance(int pane, uint32_t instanceIndex) const {
    if (pane < 0 || static_cast<size_t>(pane) >= panes_.size() ||
        panes_[static_cast<size_t>(pane)] == nullptr) {
        return true;
    }
    return instanceVisibleInPane(*panes_[static_cast<size_t>(pane)], instanceIndex);
}

void MultiView::assignNextSystemToPane(int pane, const std::string& name,
                                       size_t baseSystemCount) {
    pendingPane_ = pane;
    pendingSystemName_ = name;
    pendingBaseCount_ = baseSystemCount;
    pendingFrames_ = 0;
}

void MultiView::consumePendingAssignment() {
    if (pendingPane_ < 0 || scene_ == nullptr) {
        return;
    }
    const auto& systems = scene_->systems();
    // 场景被整体替换（载入 .mlscene）→ 旧的目标体系已不存在，直接放弃。
    if (systems.size() < pendingBaseCount_) {
        spdlog::warn("[ui] multi-view: scene replaced while assigning '{}' to view {}",
                     pendingSystemName_, pendingPane_ + 1);
        pendingPane_ = -1;
        pendingSystemName_.clear();
        return;
    }
    for (size_t i = systems.size(); i > pendingBaseCount_; --i) {
        const size_t si = i - 1;
        if (systems[si].name != pendingSystemName_) {
            continue;
        }
        const int pane = std::clamp(pendingPane_, 0, std::max(0, paneCount() - 1));
        Pane& p = ensurePane(static_cast<size_t>(pane));
        p.allSystems = false;
        p.systemIds = {systems[si].id};
        if (p.molScene != nullptr) {
            p.molScene->markDirty();
        }
        if (notifyHandler_) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), tr("已加入视图 %d：%s"), pane + 1,
                          systems[si].name.c_str());
            notifyHandler_(buf);
        }
        spdlog::info("[ui] multi-view: '{}' shown in view {}", systems[si].name,
                     pane + 1);
        pendingPane_ = -1;
        pendingSystemName_.clear();
        return;
    }
    // 解析失败 / 用户取消：最多等 30 秒（约 1800 帧）后放弃，避免永久 pending。
    if (++pendingFrames_ > 1800) {
        if (notifyHandler_) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), tr("文件已加载，但未能关联到视图 %d"),
                          pendingPane_ + 1);
            notifyHandler_(buf);
        }
        pendingPane_ = -1;
        pendingSystemName_.clear();
    }
}

void MultiView::handlePaneInput(Pane& pane, int index) {
    if (pane.camera == nullptr) {
        return;
    }
    const ImVec2 size(pane.imageMax.x - pane.imageMin.x,
                      pane.imageMax.y - pane.imageMin.y);
    if (size.x < 4.0f || size.y < 4.0f) {
        return;
    }
    ImGui::SetCursorScreenPos(pane.imageMin);
    ImGui::InvisibleButton("##pane_cam", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
        setActivePane(index);
    }

    // 每帧兜底复位拖拽状态（与 MolScene::render 的说明一致）
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) {
        pane.dragging = false;
        pane.dragButton = -1;
    } else if (pane.dragButton >= 0 && pane.dragButton <= 2 &&
               !ImGui::IsMouseDown(pane.dragButton)) {
        pane.dragging = false;
        pane.dragButton = -1;
    }

    const bool pointerHere = hovered || active;
    if (!pointerHere) {
        return;
    }
    const float sens = 0.008f;
    bool cameraChanged = false;
    for (int b = 0; b < 3; ++b) {
        if (!ImGui::IsMouseDown(b)) {
            continue;
        }
        const ImVec2 d = io.MouseDelta;
        if (std::fabs(d.x) + std::fabs(d.y) <= 0.0f) {
            continue;
        }
        if (!pane.dragging) {
            pane.dragging = true;
            pane.dragButton = b;
        }
        if (pane.dragButton != b) {
            continue;
        }
        if (b == ImGuiMouseButton_Left) {
            // 左键轨道：与 MolScene::handleMouse 完全一致的手感（分子跟随鼠标）
            pane.camera->orbit(-d.x * sens, -d.y * sens);
        } else if (b == ImGuiMouseButton_Right) {
            if (io.KeyShift) {
                const float w = std::max(size.x, 1.0f);
                const float h = std::max(size.y, 1.0f);
                pane.camera->pan({d.x / w * 2.0f, -d.y / h * 2.0f});
            } else {
                pane.camera->rollView(-d.x * 0.004f);
            }
        } else {   // 中键推拉
            pane.camera->dolly(1.0f + d.y * sens);
        }
        cameraChanged = true;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        pane.camera->dolly(1.0f - io.MouseWheel * 0.15f);
        cameraChanged = true;
    }
    if (cameraChanged && syncCameras_) {
        propagateCamera(index);
    }
    if (ImGui::IsWindowHovered() &&
        (ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
}

void MultiView::drawPaneContentMenu(Pane& pane, int index) {
    (void)index;
    if (scene_ == nullptr) {
        return;
    }
    bool all = pane.allSystems;
    if (ImGui::Checkbox(tr("全部体系（不筛选）"), &all)) {
        pane.allSystems = all;
        if (all) {
            pane.systemIds.clear();
        }
        pane.molScene->markDirty();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("选择该视图显示哪些文件/体系"));
    ImGui::BeginChild("##pane_systems", ImVec2(320.0f, 280.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoSavedSettings);
    const auto& systems = scene_->systems();
    if (systems.empty()) {
        ImGui::TextDisabled("%s", tr("（尚未载入任何体系）"));
    }
    for (size_t i = 0; i < systems.size(); ++i) {
        const entity::SystemGroup& sys = systems[i];
        ImGui::PushID(static_cast<int>(i));
        bool on = pane.allSystems || hasId(pane.systemIds, sys.id);
        if (ImGui::Checkbox(sys.name.c_str(), &on)) {
            if (pane.allSystems) {
                // 从"全部"切换到筛选：先把全部勾上，再应用本次点击
                pane.allSystems = false;
                pane.systemIds.clear();
                for (const auto& s : systems) {
                    pane.systemIds.push_back(s.id);
                }
            }
            toggleId(pane.systemIds, sys.id, on);
            if (pane.systemIds.empty()) {
                pane.allSystems = true;   // 不允许零选中（视图会永远空白）
            }
            pane.molScene->markDirty();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void MultiView::drawPaneRepMenu(
    Pane& pane, int index, const std::vector<engine::Representation>& reps) {
    (void)index;
    if (ImGui::Button(tr("全部显示"))) {
        pane.repMask.clear();
        pane.molScene->markDirty();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("只在本视图显示勾选的表示"));
    ImGui::BeginChild("##pane_reps", ImVec2(320.0f, 280.0f),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoSavedSettings);
    if (reps.empty()) {
        ImGui::TextDisabled("%s", tr("（尚未创建任何表示）"));
    }
    for (size_t i = 0; i < reps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool on = pane.repMask.empty() ||
                  i >= pane.repMask.size() || pane.repMask[i] != 0;
        char label[192];
        std::snprintf(label, sizeof(label), "%s  [%s]",
                      reps[i].name.empty() ? tr("表示") : reps[i].name.c_str(),
                      tr(engine::representationTypeName(reps[i].type)));
        if (ImGui::Checkbox(label, &on)) {
            if (pane.repMask.empty()) {
                pane.repMask.assign(reps.size(), 1u);
            }
            pane.repMask.resize(reps.size(), 1u);
            pane.repMask[i] = on ? 1u : 0u;
            if (std::none_of(pane.repMask.begin(), pane.repMask.end(),
                             [](uint8_t v) { return v != 0; })) {
                pane.repMask[i] = 1u;   // 至少保留一条，避免全黑视图
            }
            if (std::all_of(pane.repMask.begin(), pane.repMask.end(),
                            [](uint8_t v) { return v != 0; })) {
                pane.repMask.clear();   // 全勾 = 等同"不过滤"
            }
            pane.molScene->markDirty();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void MultiView::drawPaneChrome(Pane& pane, int index, const ImVec4& rect) {
    if (pane.molScene == nullptr) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 rmin(rect.x, rect.y);
    const ImVec2 rmax(rect.x + rect.z, rect.y + rect.w);
    const ImVec2 hmax(rmax.x, std::min(rmax.y, rmin.y + kHeaderH));
    const bool active = index == activePane_;

    // 标题条（图像区域之外，不会压住分子画面）+ 视图边框（当前视图高亮）
    dl->AddRectFilled(rmin, hmax, ImGui::GetColorU32(ImGuiCol_TitleBg), 0.0f);
    dl->AddRect(rmin, rmax,
                ImGui::GetColorU32(active ? ImGuiCol_CheckMark : ImGuiCol_Border),
                0.0f, 0, active ? 2.0f : 1.0f);
    if (active) {
        dl->AddRectFilled(rmin, ImVec2(rmax.x, rmin.y + 2.0f),
                          ImGui::GetColorU32(ImGuiCol_CheckMark));
    }

    // 点击标题条任意位置也切换当前视图（按钮各自处理点击，互不冲突）
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsMouseHoveringRect(rmin, hmax, false)) {
        setActivePane(index);
    }

    ImGui::PushID(index);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));
    ImGui::SetCursorScreenPos(ImVec2(rmin.x + 8.0f, rmin.y + 3.0f));

    char title[64];
    std::snprintf(title, sizeof(title), tr("视图 %d"), index + 1);
    ImGui::TextUnformatted(title);
    ImGui::SameLine();

    // 当前显示范围（内容 / 样式）按钮：popup 里勾选文件体系与表示
    char contentLabel[96];
    if (pane.allSystems) {
        std::snprintf(contentLabel, sizeof(contentLabel), "%s", tr("内容"));
    } else {
        std::snprintf(contentLabel, sizeof(contentLabel), "%s (%zu)", tr("内容"),
                      pane.systemIds.size());
    }
    if (chip(contentLabel, tr("选择该视图显示哪些文件/体系"))) {
        ImGui::OpenPopup("##pane_content_popup");
    }
    if (ImGui::BeginPopup("##pane_content_popup")) {
        drawPaneContentMenu(pane, index);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    char repLabel[96];
    std::snprintf(repLabel, sizeof(repLabel), "%s", tr("样式"));
    if (chip(repLabel, tr("只在本视图显示勾选的表示"))) {
        ImGui::OpenPopup("##pane_rep_popup");
    }
    if (ImGui::BeginPopup("##pane_rep_popup")) {
        // 表示列表在 render() 里由调用方给出；popup 内容在渲染阶段仍可访问
        // 本帧传入的副本（成员缓存），见 render() 的 cachedReps_。
        drawPaneRepMenu(pane, index, cachedReps_);
        ImGui::EndPopup();
    }

    // 打开文件并只在本视图显示它（异步解析完成后自动挂到本视图）
    const float paneW = rect.z;
    if (paneW > 260.0f) {
        ImGui::SameLine();
        if (chip(tr("打开…"), tr("把新文件加载到该视图并只显示它"))) {
            if (openFileHandler_) {
                openFileHandler_(index);
            }
        }
    }
    if (index > 0 && paneW > 330.0f) {
        ImGui::SameLine();
        if (chip(tr("对齐"), tr("把本视图相机对齐到主视图"))) {
            copyPrimaryCameraTo(index);
        }
    }

    ImGui::PopStyleVar(2);
    ImGui::PopID();
}

MultiView::RenderResult MultiView::render(
    const ImVec2& pos, const ImVec2& size,
    const std::vector<engine::Representation>& reps, uint64_t repsRevision,
    bool showAxes, bool showScale, uint64_t trajectoryFrame) {
    RenderResult result;
    if (scene_ == nullptr || primary_ == nullptr || layout_ == ViewLayout::Single) {
        return result;
    }
    if (size.x < 16.0f || size.y < 16.0f) {
        return result;
    }

    consumePendingAssignment();
    cachedReps_ = reps;
    const int count = paneCount();
    const uint32_t dirtyHint = scene_->dirtyBits();   // 整帧快照：先渲染的视图会清脏

    // —— 视图矩形 ——
    std::vector<ImVec4> rects;
    rects.reserve(static_cast<size_t>(count));
    if (layout_ == ViewLayout::SideBySide) {
        const float w = (size.x - kGap) * 0.5f;
        rects.push_back(paneRect(pos.x, pos.y, w, size.y));
        rects.push_back(paneRect(pos.x + w + kGap, pos.y, w, size.y));
    } else if (layout_ == ViewLayout::Stacked) {
        const float h = (size.y - kGap) * 0.5f;
        rects.push_back(paneRect(pos.x, pos.y, size.x, h));
        rects.push_back(paneRect(pos.x, pos.y + h + kGap, size.x, h));
    } else if (layout_ == ViewLayout::Quad) {
        const float w = (size.x - kGap) * 0.5f;
        const float h = (size.y - kGap) * 0.5f;
        rects.push_back(paneRect(pos.x, pos.y, w, h));
        rects.push_back(paneRect(pos.x + w + kGap, pos.y, w, h));
        rects.push_back(paneRect(pos.x, pos.y + h + kGap, w, h));
        rects.push_back(paneRect(pos.x + w + kGap, pos.y + h + kGap, w, h));
    } else {
        rects.push_back(paneRect(pos.x, pos.y, size.x, size.y));
    }

    for (int i = 0; i < count; ++i) {
        Pane& pane = ensurePane(static_cast<size_t>(i));
        if (pane.molScene == nullptr || pane.camera == nullptr) {
            continue;
        }
        if (!pane.primary && pane.renderer) {
            // 表示列表零拷贝共享（绑定只在指针/修订号变化时生效）
            pane.renderer->bindRepresentations(&reps, repsRevision);
        }
        // 先确定本帧的图像矩形：取景（framePane）要用它的宽高比做横向适配，
        // 否则窄视口里"按半径取景"只保证竖向装得下，横向仍会被裁掉。
        const ImVec4& r = rects[static_cast<size_t>(i)];
        const ImVec2 imagePos(r.x, r.y + kHeaderH);
        const ImVec2 imageSize(std::max(r.z, 1.0f), std::max(r.w - kHeaderH, 1.0f));
        pane.imageMin = imagePos;
        pane.imageMax = ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y);

        bool instanceChanged = false;
        if (applyPaneFilter(pane, reps, &instanceChanged)) {
            pane.molScene->markDirty();
            // 文件/体系筛选变化后自动把该视图取景到它显示的内容：否则筛掉大半
            // 分子后画面会停在原来的全景位置，新视图里只剩角落里一个小点。
            if (instanceChanged) {
                framePane(i);
            }
        }
        // 布局切换（1↔2↔4）后每个视图按自己的可见内容重新取景一次：视图尺寸
        // 变了、内容范围也可能不同，沿用旧相机容易只看到空白或局部。
        if (refitOnLayoutChange_) {
            framePane(i);
        }
        pane.molScene->setTrajectoryFrame(trajectoryFrame);
        pane.molScene->setViewOverlay(showAxes, showScale);

        ImGui::SetCursorScreenPos(imagePos);
        // 主视图走正常脏位路径（它负责清脏）；副视图显式喂同一份快照。
        pane.molScene->render(imageSize, pane.primary ? 0u : dirtyHint);
        // 每个视图的相机隐形按钮/标题条控件必须在独立 ID 栈里：两个视图都叫
        // "##pane_cam" 会在同一窗口里产生冲突 ID（ImGui 调试层直接弹红色警告）。
        ImGui::PushID(i);
        handlePaneInput(pane, i);
        drawPaneChrome(pane, i, r);
        ImGui::PopID();

        if (i == 0) {
            result.rendered = true;
            result.primaryMin = pane.imageMin;
            result.primaryMax = pane.imageMax;
        }
    }
    refitOnLayoutChange_ = false;
    return result;
}

} // namespace moleculens::app
