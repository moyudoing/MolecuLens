// =============================================================================
// MolecuLens · app · view/overlay/loading_overlay.h —— 加载遮罩（状态 + 绘制）
// 规格来源：主方案 §7.5（全屏半透明黑幕 + 居中卡片：文件名/大小/阶段/进度条；
//           完成或失败后 0.5–1s 倒计时关闭）
//
// 从 view_control.cpp 抽出的独立关注点：**加载遮罩**不依赖窗口生命周期或控制器，
// 只需一份状态 + 一个绘制入口。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace moleculens::app::overlay {

// 加载阶段（主方案 §7.5）
enum class LoadingStage { Queued, ReadingFile, AnalyzingBonds, UploadingGPU, Done, Failed };

struct LoadingState {
    LoadingStage stage = LoadingStage::Done;
    std::string  fileName;
    uint64_t     fileSize = 0;
    float        progress = -1.0f;      // <0 表示不确定进度
    int          delayFrames = 0;       // 完成/失败后的关闭倒计时
};

// 绘制遮罩。active = 是否需要绘制；本函数负责在完成/失败后倒计时并自行把
// active 置 false（调用方无需再判断阶段）。state 会被就地更新（delayFrames）。
void render(LoadingState& state, bool& active);

} // namespace moleculens::app::overlay
