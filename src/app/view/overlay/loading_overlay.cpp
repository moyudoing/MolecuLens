// =============================================================================
// MolecuLens · app · view/overlay/loading_overlay.cpp —— 加载遮罩实现
// 规格来源：主方案 §7.5
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/overlay/loading_overlay.h"

#include <cstdio>

#include <imgui.h>

#include "app/view/theme/ui_theme.h"

namespace moleculens::app::overlay {

void render(LoadingState& state, bool& active) {
    if (!active) {
        return;
    }
    // 全屏半透明黑幕（主方案 §7.5：alpha 0.55 + NoInputs 屏蔽交互）
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##loading_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    // 居中卡片（文件名 + 大小 + 阶段指示 + 进度条）
    const float cardW = 420.0f, cardH = 150.0f;
    ImGui::SetCursorPos(ImVec2((viewport->Size.x - cardW) * 0.5f,
                               (viewport->Size.y - cardH) * 0.5f));
    ImGui::BeginChild("##loading_card", ImVec2(cardW, cardH), true);
    ImGui::TextUnformatted(state.fileName.c_str());
    ImGui::SameLine();
    // formatFileSize（主方案 §7.5：人性化 KB/MB）
    char sizeBuf[64];
    const double mb = static_cast<double>(state.fileSize) / (1024.0 * 1024.0);
    if (mb >= 1.0) {
        std::snprintf(sizeBuf, sizeof(sizeBuf), "(%.1f MB)", mb);
    } else {
        std::snprintf(sizeBuf, sizeof(sizeBuf), "(%.1f KB)",
                      static_cast<double>(state.fileSize) / 1024.0);
    }
    ImGui::TextDisabled("%s", sizeBuf);
    ImGui::Separator();
    static const char* stageNames[] = {tr("排队中"), tr("读取文件"), tr("分析键"), tr("上传 GPU"),
                                       tr("完成"), tr("失败")};
    ImGui::Text(tr("阶段：%s"), stageNames[static_cast<int>(state.stage)]);
    ImGui::Dummy(ImVec2(0, 8));
    UITheme::loadingBar(state.progress, ImVec2(cardW - 40, 8));
    ImGui::EndChild();
    ImGui::End();

    // 完成/失败倒计时关闭（主方案 §7.5：0.5–1s ≈ 45 帧）
    if (state.stage == LoadingStage::Done || state.stage == LoadingStage::Failed) {
        if (++state.delayFrames > 45) {
            active = false;
        }
    }
}

} // namespace moleculens::app::overlay
