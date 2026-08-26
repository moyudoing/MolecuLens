// =============================================================================
// MolecuLens · app · view/tools/mol_list/mol_list_tool.cpp —— 分子列表面板实现
// 规格来源：主方案 §7.9；扩展：以体系为管理单位——体系树（折叠/显隐/重命名/
//           删除）、成员列表（删除/跨组移交/新建体系并移动）、体系/分子位置
//           （平移、定位）编辑。修复：删除后选中索引回退、空场景提示、此前
//           仅列出平铺分子且删除为空桩。
// =============================================================================

#include "app/view/tools/mol_list/mol_list_tool.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>

#include "app/controller/scene/scene_controller.h"
#include "app/data/data_manager.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"
#include "entity/scene/scene.h"

#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

// 显示名兜底（体系/分子无自定义名时）
std::string displayName(const std::string& name, const char* kind, size_t index) {
    if (!name.empty()) {
        return name;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_%zu", kind, index);
    return buf;
}

} // namespace

void MolListTool::render(DataManager* dm, MolScene* scene) {
    if (!dm || !sceneCtl_) {
        ImGui::TextDisabled("控制器未就绪");
        return;
    }
    const size_t sysCount = dm->systemCount();
    if (sysCount == 0) {
        ImGui::TextDisabled("暂无体系（Ctrl+O 打开 PDB/GRO）");
        if (ImGui::Button("+ 新建体系")) {
            sceneCtl_->addSystem("");
        }
        return;
    }
    // 选中/展开索引越界防护（体系删除后回退）
    if (selectedSystem_ >= sysCount) {
        selectedSystem_ = sysCount - 1;
    }
    if (selectedInstance_ >= 0 &&
        static_cast<size_t>(selectedInstance_) >= dm->moleculeCount()) {
        selectedInstance_ = -1;
    }
    if (expandedSystem_ >= sysCount) {
        expandedSystem_ = SIZE_MAX;
    }

    if (ImGui::Button("+ 新建体系")) {
        sceneCtl_->addSystem("");
        selectedSystem_ = sysCount;   // 新体系在末尾（下一帧 clamp）
        selectedInstance_ = -1;
        expandedSystem_ = SIZE_MAX;
    }
    ImGui::Separator();

    ImGui::BeginChild("##sys_list_scroll");   // 可滚动，支持大量体系/分子
    for (size_t si = 0; si < sysCount; ++si) {
        const uint32_t sysId = dm->systemId(si);
        ImGui::PushID(static_cast<int>(si * 1000 + 1));

        // —— 体系头行：可见性 + 名称（点击选中/展开）——
        bool vis = dm->systemVisible(si);
        if (ImGui::Checkbox("##sysvis", &vis)) {
            sceneCtl_->setSystemVisible(sysId, vis);
        }
        ImGui::SameLine();
        std::string name = displayName(dm->systemName(si), "system", si);
        const bool isCur = (selectedSystem_ == si);
        const bool expanded = (expandedSystem_ == si);
        std::string header = (expanded ? "▼ " : "▶ ") + name;
        if (ImGui::Selectable(header.c_str(), isCur)) {
            selectedSystem_ = si;
            selectedInstance_ = -1;
            expandedSystem_ = (expanded ? SIZE_MAX : si);   // 再次点击收起
        }
        // 体系右键：重命名 / 定位 / 删除
        if (ImGui::BeginPopupContextItem("sys_ctx")) {
            if (ImGui::MenuItem("重命名")) {
                renameSystemIndex_ = static_cast<int>(si);
                std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", name.c_str());
            }
            if (ImGui::MenuItem("体系定位到原点")) {
                sceneCtl_->setSystemPosition(sysId, glm::vec3(0.0f));
            }
            // 体系独立渲染模式（实心/线框/透明/点；互不影响）
            if (ImGui::BeginMenu("渲染模式")) {
                static const char* modeNames[] = {"实心", "线框", "透明", "点"};
                int curMode = 0;
                if (const auto* scp = scene ? scene->scene() : nullptr) {
                    if (si < scp->systems().size()) {
                        curMode = static_cast<int>(scp->systems()[si].renderMode);
                    }
                }
                for (int m = 0; m < 4; ++m) {
                    if (ImGui::MenuItem(modeNames[m], nullptr, curMode == m)) {
                        sceneCtl_->setSystemRenderMode(sysId, m);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("删除体系")) {
                spdlog::info("[ui] 删除体系 {}", name);
                sceneCtl_->removeSystem(sysId);
                if (selectedSystem_ >= si && selectedSystem_ > 0) {
                    --selectedSystem_;
                }
                selectedInstance_ = -1;
                expandedSystem_ = SIZE_MAX;
            }
            ImGui::EndPopup();
        }
        // 内联重命名输入（Enter 提交）
        if (renameSystemIndex_ == static_cast<int>(si)) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##rename", renameBuf_, sizeof(renameBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                sceneCtl_->renameSystem(sysId, renameBuf_);
                renameSystemIndex_ = -1;
            }
        }

        // —— 展开的成员分子列表（同构聚合：md.gro 中多个 SOL 合并显示一条计数）——
        if (expandedSystem_ == si) {
            ImGui::Indent();
            const size_t mc = dm->systemMemberCount(si);
            if (mc == 0) {
                ImGui::TextDisabled("（空体系）");
            }
            // 按 (structureIndex) 聚合：同一体系内相同原子结构合并显示为一条带计数项
            struct Group { uint32_t structureIndex = 0; uint32_t firstInst = 0; size_t count = 0; };
            std::vector<Group> groups;
            for (size_t m = 0; m < mc; ++m) {
                const uint32_t inst = dm->systemMemberInstance(si, m);
                if (inst == UINT32_MAX || inst >= dm->moleculeCount()) {
                    continue;
                }
                const auto* scp_ = scene ? scene->scene() : nullptr;
                if (!scp_ || inst >= scp_->instances().size()) {
                    continue;
                }
                const uint32_t siIdx = scp_->instances()[inst].structureIndex;
                Group* g = nullptr;
                for (auto& gg : groups) {
                    if (gg.structureIndex == siIdx) { g = &gg; break; }
                }
                if (!g) { groups.push_back({siIdx, inst, 0}); g = &groups.back(); }
                ++g->count;
            }
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                const uint32_t inst = groups[gi].firstInst;
                ImGui::PushID(static_cast<int>(gi * 97 + 2));
                std::string mname = displayName(dm->moleculeName(inst), "molecule", inst);
                char row[128];
                if (groups[gi].count > 1) {
                    std::snprintf(row, sizeof(row), "%s × %zu", mname.c_str(), groups[gi].count);
                } else {
                    std::snprintf(row, sizeof(row), "%s", mname.c_str());
                }
                const bool mCur = (selectedInstance_ == static_cast<int>(inst));
                if (ImGui::Selectable(row, mCur)) {
                    selectedSystem_ = si;
                    selectedInstance_ = static_cast<int>(inst);
                }
                if (mCur) {
                    char info[128];
                    std::snprintf(info, sizeof(info), "原子 %zu · 键 %zu · 帧 %zu",
                                  dm->atomCount(inst), dm->bondCount(inst),
                                  dm->frameCount(inst));
                    ImGui::TextDisabled("%s", info);
                }
                // 分子/组右键：定位 / 删除（组删除全部）/ 跨组移交（组移交全部）
                if (ImGui::BeginPopupContextItem("mol_ctx")) {
                    if (ImGui::MenuItem("定位到原点")) {
                        sceneCtl_->setInstancePosition(inst, glm::vec3(0.0f));
                    }
                    if (groups[gi].count > 1) {
                        if (ImGui::MenuItem("删除全部")) {
                            const auto* scp2_ = scene ? scene->scene() : nullptr;
                            for (int pass = 0; pass < 2; ++pass) {
                                const size_t cc = dm->systemMemberCount(si);
                                std::vector<uint32_t> victims;
                                for (size_t m = 0; m < cc; ++m) {
                                    const uint32_t ii = dm->systemMemberInstance(si, m);
                                    if (ii >= dm->moleculeCount()) continue;
                                    if (!scp2_ || ii >= scp2_->instances().size()) continue;
                                    if (scp2_->instances()[ii].structureIndex == groups[gi].structureIndex) {
                                        victims.push_back(ii);
                                    }
                                }
                                if (victims.empty()) break;
                                std::sort(victims.begin(), victims.end(), std::greater<uint32_t>());
                                for (uint32_t v : victims) sceneCtl_->removeInstance(v);
                                break;
                            }
                            selectedInstance_ = -1;
                        }
                    } else if (ImGui::MenuItem("删除")) {
                        spdlog::info("[ui] 删除分子 {}", inst);
                        sceneCtl_->removeInstance(inst);
                        selectedInstance_ = -1;
                    }
                    ImGui::Separator();
                    if (ImGui::BeginMenu("移动到体系")) {
                        for (size_t ts = 0; ts < sysCount; ++ts) {
                            if (ts == si) continue;
                            std::string tn = displayName(dm->systemName(ts), "system", ts);
                            if (ImGui::MenuItem(tn.c_str())) {
                                const auto* scp3_ = scene ? scene->scene() : nullptr;
                                if (groups[gi].count > 1) {
                                    const size_t cc = dm->systemMemberCount(si);
                                    std::vector<uint32_t> mov;
                                    for (size_t m = 0; m < cc; ++m) {
                                        const uint32_t ii = dm->systemMemberInstance(si, m);
                                        if (ii >= dm->moleculeCount()) continue;
                                        if (!scp3_ || ii >= scp3_->instances().size()) continue;
                                        if (scp3_->instances()[ii].structureIndex == groups[gi].structureIndex) {
                                            mov.push_back(ii);
                                        }
                                    }
                                    std::sort(mov.begin(), mov.end(), std::greater<uint32_t>());
                                    for (uint32_t v : mov) sceneCtl_->moveInstanceToSystem(v, dm->systemId(ts));
                                } else {
                                    sceneCtl_->moveInstanceToSystem(inst, dm->systemId(ts));
                                }
                            }
                        }
                        if (ImGui::MenuItem("新建体系并移动")) {
                            const uint32_t nid = sceneCtl_->addSystem("");
                            if (groups[gi].count > 1) {
                                const auto* scp4_ = scene ? scene->scene() : nullptr;
                                const size_t cc = dm->systemMemberCount(si);
                                std::vector<uint32_t> mov;
                                for (size_t m = 0; m < cc; ++m) {
                                    const uint32_t ii = dm->systemMemberInstance(si, m);
                                    if (ii >= dm->moleculeCount()) continue;
                                    if (!scp4_ || ii >= scp4_->instances().size()) continue;
                                    if (scp4_->instances()[ii].structureIndex == groups[gi].structureIndex) {
                                        mov.push_back(ii);
                                    }
                                }
                                std::sort(mov.begin(), mov.end(), std::greater<uint32_t>());
                                for (uint32_t v : mov) sceneCtl_->moveInstanceToSystem(v, nid);
                            } else {
                                sceneCtl_->moveInstanceToSystem(inst, nid);
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // —— 位置编辑（选中体系 / 分子，平移 + 定位，任务一.5）——
    const auto* sc = scene ? scene->scene() : nullptr;
    if (!sc) {
        return;
    }
    if (selectedSystem_ < sysCount) {
        const uint32_t sysId = dm->systemId(selectedSystem_);
        const uint32_t si2 = sc->systemIndexById(sysId);
        if (si2 != UINT32_MAX) {
            UITheme::sectionHeader("体系位置");
            glm::vec3 pos = sc->systems()[si2].transform.translation;
            if (ImGui::DragFloat3("平移", &pos.x, 0.5f)) {
                sceneCtl_->setSystemPosition(sysId, pos);
            }
            if (UITheme::primaryButton("体系定位到原点")) {
                sceneCtl_->setSystemPosition(sysId, glm::vec3(0.0f));
            }
        }
    }
    if (selectedInstance_ >= 0 &&
        static_cast<size_t>(selectedInstance_) < sc->instances().size()) {
        UITheme::sectionHeader("分子位置");
        glm::vec3 pos = sc->instances()[selectedInstance_].transform.translation;
        if (ImGui::DragFloat3("平移", &pos.x, 0.5f)) {
            sceneCtl_->setInstancePosition(static_cast<uint32_t>(selectedInstance_), pos);
        }
        if (UITheme::primaryButton("分子定位到原点")) {
            sceneCtl_->setInstancePosition(static_cast<uint32_t>(selectedInstance_),
                                           glm::vec3(0.0f));
        }
    }
}

} // namespace moleculens::app
