// =============================================================================
// MolecuLens · engine/export/anim/anim_encoder.h —— 动画封装（PNG 序列 → 视频/动图）
//
// 设计：逐帧渲染仍由渲染器负责（MolScene / CLI OffscreenRenderer），本模块只做
//       "把 frame_%04d.png 交给外部编码器"这一件事。
// 依赖：ffmpeg（PATH 中找到，或用环境变量 MOLECULENS_FFMPEG 指定完整路径）。
//       未安装 ffmpeg 时 PNG 序列仍可用，只是无法封装成 MP4/WebM/GIF/APNG。
// =============================================================================

#pragma once

#include <string>

namespace moleculens::engine {

// 目标封装格式（由输出文件扩展名推断，也可显式指定）
enum class AnimFormat { Apng, Gif, Mp4, Webm };

struct AnimEncodeOptions {
    // 输入序列：含 %04d 的路径，例如 "export/anim.frames/frame_%04d.png"
    std::string framePattern;
    // 输出文件：扩展名决定格式（.mp4/.webm/.gif/.apng/.png）
    std::string outPath;
    AnimFormat format = AnimFormat::Mp4;
    int fps = 30;
    bool loop = true;          // GIF/APNG 无限循环
    int startNumber = 0;       // 首帧编号（与我们的顺序命名一致，默认 0）
};

// 依扩展名推断格式；未知扩展名返回 Mp4
AnimFormat animFormatFromPath(const std::string& path);
// 该格式对应的扩展名（含点），如 ".mp4"
const char* animFormatExtension(AnimFormat format);
const char* animFormatName(AnimFormat format);

// 定位 ffmpeg：环境变量 MOLECULENS_FFMPEG → PATH（where ffmpeg）→ 当前目录。
// 找不到返回空串。
std::string findFfmpegExecutable();

// 编码。失败时把 ffmpeg 的最后几行输出写入 *error（可为 nullptr）。
bool encodeAnimation(const AnimEncodeOptions& opts, std::string* error);

} // namespace moleculens::engine
