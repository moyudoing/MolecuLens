// =============================================================================
// MolecuLens · app · view/tools/export/export_img_tool.cpp —— 导出面板实现
// 规格来源：主方案 §7.9/§12.5（UI 就绪；执行链路 M4 接入 offscreen + 编码器）
// =============================================================================

#include "app/i18n/language.h"
#include "app/view/tools/export/export_img_tool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "app/view/control/view_control.h"
#include "app/view/theme/ui_theme.h"
#include "app/view/viewport/mol_scene.h"
#include "engine/export/anim/anim_encoder.h"

#include "spdlog/spdlog.h"

namespace moleculens::app {

namespace {

// GUI 导出的默认落点：用户「图片」目录（不存在则 Documents / 临时目录）。
// 与 ViewControl::saveFileDialogEx 的初始目录保持同一策略，避免“相对路径 +
// 进程 cwd 不可写”导致的“没有权限”。
std::string defaultOutputPath(const char* leaf) {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home != nullptr && *home != '\0') {
        std::filesystem::path base(home);
        for (const char* sub : {"Pictures", "Documents"}) {
            std::filesystem::path dir = base / sub / "MolecuLens";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (!ec && std::filesystem::is_directory(dir)) {
                return (dir / leaf).string();
            }
        }
    }
#endif
    std::error_code ec;
    std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "MolecuLens";
    std::filesystem::create_directories(dir, ec);
    return (dir / leaf).string();
}

std::string resolveOutputPath(const std::string& path, const char* leaf) {
    if (path.empty()) {
        return defaultOutputPath(leaf);
    }
    const std::filesystem::path p(path);
    if (p.is_relative()) {
        // 相对路径按默认的用户目录解析，而不是按进程 cwd
        return (std::filesystem::path(defaultOutputPath("")) / p).string();
    }
    return path;
}

}   // namespace

ExportImgTool::~ExportImgTool() {
    // 后台导出线程 join（应用关闭时确保导出完成后再销毁 MolScene/渲染器）
    if (exportThread_.joinable()) {
        exportThread_.join();
    }
}

void ExportImgTool::render(DataManager* dm, MolScene* scene) {
    (void)dm;
    if (ImGui::BeginTabBar("export_tabs")) {
        // —— 高 DPI 图像页签（主方案 §12.5）——
        if (ImGui::BeginTabItem(tr("高 DPI 图像"))) {
            // 面板状态是成员（见头注释）：所有控件**直接绑定** imgOpts_ 的字段，
            // 不存在"UI 局部值 vs 生效参数"两份状态，也就不会再出现两者不一致。
            // 默认质量参数（ssaa=2 / rtSamples=4 / scale=2）已直接写在
            // ImageExportOptions 的字段初值里 —— 此前是"结构体默认值 + 首次进入
            // 面板时才提升"两份来源，任何不经面板的路径（CLI、动画导出）都拿不到
            // 提升后的值。单一来源后不存在这个分叉。
            ImageExportOptions& opts = imgOpts_;

            ImGui::Text(tr("渲染方式"));
            ImGui::SameLine();
            if (ImGui::Combo("##rendermode", &renderModeIdx_,
                             trItems({"光栅化 (Vulkan)", "光线追踪 (CPU)"}).c_str())) {
                opts.rayTraced = (renderModeIdx_ == 1);
            }
            if (opts.rayTraced) {
                if (scene != nullptr) {
                    ImGui::TextDisabled("%s",
                        scene->rayTraceSupported()
                            ? tr("GPU 光追扩展可用（导出后端仍为 CPU，结果一致）")
                            : tr("未检测到 GPU 光追扩展，使用 CPU 光追"));
                }
                ImGui::SliderInt(tr("RT 采样"), &opts.rtSamples, 1, 8, "%d");
                UITheme::helpMarker(tr("光线追踪：方向光软阴影 + 球体反射；采样数越高越平滑、"
                                       "越慢（推荐 4-8）"));
            } else {
                ImGui::SliderInt(tr("抗锯齿 (SSAA)"), &opts.ssaa, 1, 8, "%d");
                UITheme::helpMarker(tr("光栅化导出：超采样倍数（先渲染高分辨率再降采样，"
                                       "消除锯齿边缘；推荐 2-4，数值越大越慢）"));
            }
            ImGui::Text(tr("尺寸模式"));
            ImGui::SameLine();
            if (ImGui::Combo("##sizemode", &sizeModeIdx_,
                             trItems({"视口倍数", "绝对尺寸", "预设分辨率"}).c_str())) {
                // 视口倍数 = 相对；绝对尺寸/预设分辨率 = 绝对像素
                opts.sizeMode =
                    sizeModeIdx_ == 0 ? SizeMode::ViewportScale : SizeMode::Absolute;
            }
            if (sizeModeIdx_ == 0) {
                ImGui::SliderFloat(tr("倍数"), &opts.scale, 1.0f, 8.0f, "%.1fx");
                UITheme::helpMarker(tr("相对视口尺寸的倍数（2x = 高清，4x = 超高清）"));
            } else if (sizeModeIdx_ == 1) {
                ImGui::InputScalar(tr("宽"), ImGuiDataType_U32, &opts.width);
                ImGui::InputScalar(tr("高"), ImGuiDataType_U32, &opts.height);
            } else {
                const char* presetNames[] = {
                    "Full HD (1920x1080)",
                    "WQHD (2560x1440)",
                    "4K UHD (3840x2160)",
                    "5K (5120x2880)",
                    "8K UHD (7680x4320)",
                };
                const uint32_t presetSizes[][2] = {
                    {1920, 1080},
                    {2560, 1440},
                    {3840, 2160},
                    {5120, 2880},
                    {7680, 4320},
                };
                ImGui::Text(tr("分辨率"));
                ImGui::SameLine();
                if (ImGui::Combo("##preset", &presetIdx_, presetNames, 5)) {
                    opts.width = presetSizes[presetIdx_][0];
                    opts.height = presetSizes[presetIdx_][1];
                }
                ImGui::Text(tr("  宽: %u  高: %u"), opts.width, opts.height);
            }
            // 背景：直接绑定枚举（此前用 int 局部变量 + 导出时才换算）
            int bgIdx = opts.background == Bg::Current ? 0
                                                       : (opts.background == Bg::White ? 1 : 2);
            ImGui::Text(tr("背景"));
            ImGui::SameLine();
            if (ImGui::Combo("##bg", &bgIdx, trItems({"当前背景", "白色", "透明"}).c_str())) {
                opts.background = bgIdx == 0 ? Bg::Current
                                             : (bgIdx == 1 ? Bg::White : Bg::Transparent);
            }
            UITheme::helpMarker(tr("透明：清屏 alpha=0，几何写自身 alpha -> PNG 可直接叠加合成"));
            int depthIdx = opts.colorDepth == 16 ? 1 : 0;
            ImGui::Text(tr("位深"));
            ImGui::SameLine();
            // 字符串里的 \0 是 ImGui 的多项分隔符，不是文本结束符。
            // 写成 "8-bit\016-bit (PNG)\0" 时 \016 会被解析为八进制转义(0x0E)，
            // 第二项实际显示成 "6-bit (PNG)"；拆成相邻字面量即可避开。
            if (ImGui::Combo("##depth", &depthIdx, "8-bit\0" "16-bit (PNG)\0")) {
                opts.colorDepth = depthIdx == 0 ? 8 : 16;
            }
            UITheme::helpMarker(tr("16-bit 的真实来源是 SSAA：多个样本在浮点上求平均后再量化，" "精度高于 8-bit（SSAA=1 时仅容器变宽，无额外精度）"));
            ImGui::SliderInt("MSAA", &opts.msaa, 0, 8, "%d");
            UITheme::helpMarker(tr("多重采样抗锯齿。注意：离屏 FBO 附件恒为 1x（管线/渲染通道" "一致性的硬约束），该参数当前不影响离屏导出——" "抗锯齿请用上方的 SSAA"));
            int fitIdx = static_cast<int>(opts.fit);
            ImGui::Text(tr("相机适配"));
            ImGui::SameLine();
            if (ImGui::Combo("##fit", &fitIdx, "Contain\0Stretch\0Crop\0")) {
                opts.fit = static_cast<Fit>(fitIdx);
            }
            ImGui::InputScalar("DPI", ImGuiDataType_U32, &opts.dpi);
            // ESP 叠加说明：色标条/极值标注在 PNG 写出前栅格化进像素（engine
            // esp_overlay_raster）；透明背景 alpha=0 与 16-bit HDR 路径无法叠加。
            ImGui::TextDisabled("%s",
                                tr("ESP 色标/极值标注：导出 PNG 自动叠加（透明背景与 16-bit HDR 路径除外）。"));
            // 默认文件名（仅作保存对话框的初始名；位置/最终文件名由
            // 系统原生"另存为"对话框决定——替代原固定路径导出）
            ImGui::InputText(tr("默认文件名"), fileName_, sizeof(fileName_));
            ImGui::BeginDisabled(exportRunning_);
            if (UITheme::primaryButton(exportRunning_ ? tr("导出中…") : tr("导出图像..."))) {
                // 弹出系统原生保存对话框（Windows 资源管理器；PNG 过滤器）。
                // 用户取消 → 返回空串 → 中止导出、不生成文件。
                const std::string chosen = ViewControl::saveFileDialog(fileName_);
                if (chosen.empty()) {
                    spdlog::info("[io] export cancelled by user (no file written)");
                } else {
                    opts.outPath = chosen;
                    startExport(opts, scene);   // QS.4：后台线程导出，不阻塞主视图
                }
            }
            ImGui::EndDisabled();
            // 导出状态（后台执行，主视图可继续操作）
            if (exportRunning_) {
                ImGui::TextDisabled(tr("后台导出中… 主视图可继续操作"));
            } else if (exportFinished_) {
                std::string msg;
                {
                    std::lock_guard<std::mutex> lk(msgMutex_);
                    msg = exportMsg_;
                }
                if (!msg.empty()) {
                    ImGui::TextDisabled("%s", msg.c_str());
                }
            }
            ImGui::EndTabItem();
        }
        // —— 动画导出页签（主方案 §12.6）——
        if (ImGui::BeginTabItem(tr("动画导出"))) {
            AnimationExportOptions& opts = animOpts_;
            int src = opts.source;
            ImGui::Text(tr("动画源"));
            ImGui::SameLine();
            if (ImGui::Combo("##src", &src, trItems({"轨迹播放", "相机旋转", "轨迹+相机"}).c_str())) {
                opts.source = src;
            }
            int codec = static_cast<int>(opts.codec);
            ImGui::Text(tr("编解码器"));
            ImGui::SameLine();
            if (ImGui::Combo("##codec", &codec,
                             trItems({"PNG 序列", "APNG", "GIF", "MP4 (ffmpeg)", "WebM (ffmpeg)"}).c_str())) {
                opts.codec = static_cast<Codec>(codec);
            }
            // 帧范围只对轨迹有意义；纯相机旋转用“旋转帧数”（复用结束帧字段）。
            if (opts.source == 1) {
                ImGui::InputScalar(tr("旋转帧数"), ImGuiDataType_U64, &opts.outFrame);
                ImGui::TextDisabled("%s", tr("0 = 默认 120 帧；绕当前视轴转一整圈"));
            } else {
                ImGui::InputScalar(tr("起始帧"), ImGuiDataType_U64, &opts.inFrame);
                ImGui::InputScalar(tr("结束帧"), ImGuiDataType_U64, &opts.outFrame);
            }
            ImGui::InputFloat("fps", &opts.fps);
            ImGui::Checkbox(tr("循环"), &opts.loop);
            ImGui::InputScalar(tr("宽度倍数"), ImGuiDataType_U32, &opts.scale);
            ImGui::SliderInt(tr("抗锯齿 (SSAA)"), &opts.ssaa, 1, 8, "%d");
            int animDepthIdx = opts.colorDepth == 16 ? 1 : 0;
            ImGui::Text(tr("位深"));
            ImGui::SameLine();
            // \0 是 ImGui 多项分隔符，不是转义终止（同上方"位深"下拉框的说明）
            if (ImGui::Combo("##animdepth", &animDepthIdx, "8-bit\0" "16-bit (PNG)\0")) {
                opts.colorDepth = animDepthIdx == 0 ? 8 : 16;
            }
            UITheme::helpMarker(tr("逐帧导出与单帧导出共用同一抗锯齿/位深链路；" "帧数多时 SSAA 会成倍拉长导出时间"));
            ImGui::InputText(tr("输出路径"), animPath_, sizeof(animPath_));
            if (opts.codec != Codec::PngSequence) {
                ImGui::SameLine();
                if (ImGui::Button(tr("浏览..."))) {
                    const char* filter = nullptr;
                    const char* ext = "mp4";
                    switch (opts.codec) {
                    case Codec::Apng:
                        filter = "APNG (*.apng)\0*.apng\0所有文件 (*.*)\0*.*\0";
                        ext = "apng";
                        break;
                    case Codec::Gif:
                        filter = "GIF (*.gif)\0*.gif\0所有文件 (*.*)\0*.*\0";
                        ext = "gif";
                        break;
                    case Codec::Webm:
                        filter = "WebM (*.webm)\0*.webm\0所有文件 (*.*)\0*.*\0";
                        ext = "webm";
                        break;
                    case Codec::Mp4:
                    case Codec::PngSequence:
                    default:
                        filter = "MP4 (*.mp4)\0*.mp4\0所有文件 (*.*)\0*.*\0";
                        ext = "mp4";
                        break;
                    }
                    const std::filesystem::path current(animPath_);
                    std::string name = current.filename().string();
                    if (name.empty()) {
                        name = std::string("trajectory.") + ext;
                    }
                    const std::string chosen =
                        ViewControl::saveFileDialogEx(name.c_str(), filter, ext);
                    if (!chosen.empty()) {
                        std::snprintf(animPath_, sizeof(animPath_), "%s", chosen.c_str());
                    }
                }
            }
            UITheme::helpMarker(tr("视频/GIF 用系统“另存为”选择目标文件；PNG 序列填目录。"
                                   "留空时自动写到“图片/MolecuLens”。"));
            ImGui::BeginDisabled(exportRunning_);
            if (UITheme::primaryButton(tr("导出动画"))) {
                opts.outPath = resolveOutputPath(
                    animPath_, opts.codec == Codec::PngSequence ? "trajectory_frames"
                                                                : "trajectory.mp4");
                startAnimationExport(opts, scene);   // 后台线程，不阻塞主视图
            }
            ImGui::EndDisabled();
            if (exportRunning_) {
                ImGui::TextDisabled(tr("后台导出中… 主视图可继续操作"));
            } else if (exportFinished_) {
                std::string msg;
                {
                    std::lock_guard<std::mutex> lk(msgMutex_);
                    msg = exportMsg_;
                }
                if (!msg.empty()) {
                    ImGui::TextDisabled("%s", msg.c_str());
                }
            }
            ImGui::TextDisabled(tr("PNG 序列直接输出；APNG/GIF/MP4/WebM 调用 ffmpeg（PATH 或 MOLECULENS_FFMPEG）"));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

bool ExportImgTool::exportImage(const ImageExportOptions& opts, MolScene* scene) {
    if (!scene) {
        spdlog::error("[io] export: 场景未就绪");
        return false;
    }
    uint32_t w = 0, h = 0;
    if (opts.sizeMode == SizeMode::ViewportScale) {
        w = static_cast<uint32_t>(std::max(1u, scene->displayWidth()) * opts.scale);
        h = static_cast<uint32_t>(std::max(1u, scene->displayHeight()) * opts.scale);
    } else {
        w = std::max(1u, opts.width);
        h = std::max(1u, opts.height);
    }
    const float* bg = nullptr;
    const float white[3] = {1.0f, 1.0f, 1.0f};
    bool transparentBg = false;
    if (opts.background == Bg::White) {
        bg = white;
    } else if (opts.background == Bg::Transparent) {
        // 透明背景由离屏清屏 alpha=0 实现（几何仍以不透明管线绘制，写自身 alpha）；
        // 背景 RGB 取白色作为底，alpha=0 时颜色通道不参与合成。
        bg = white;
        transparentBg = true;
    }
    const std::string out = opts.outPath.empty() ? "export/image.png" : opts.outPath;
    spdlog::info("[io] export image {} -> {} ({}x{}, {} bit)", scene->displayWidth(),
                 out, w, h, opts.colorDepth);
    if (opts.rayTraced) {
        // 光追路径无 SSAA 超采样，16-bit 无额外精度可言 → 固定 8-bit 输出
        return scene->exportRayTracedToPng(w, h, out, bg, opts.rtSamples);
    }
    return scene->exportToPng(w, h, out, bg, opts.ssaa, opts.colorDepth, transparentBg);
}

bool ExportImgTool::exportAnimation(const AnimationExportOptions& opts, MolScene* scene) {
    if (scene == nullptr) {
        spdlog::error("[io] export(anim): 场景未就绪");
        return false;
    }
    // 尺寸：以视口逻辑尺寸 × scale 为准（与图像导出的"视口倍数"一致）
    const uint32_t w = std::max(1u, scene->displayWidth() * std::max(1u, opts.scale));
    const uint32_t h = std::max(1u, scene->displayHeight() * std::max(1u, opts.scale));
    // 纯相机旋转不依赖轨迹帧；轨迹+相机仍按轨迹帧推进，同时叠加相机环绕。
    const uint32_t orbitFrames = static_cast<uint32_t>(
        opts.outFrame == 0 ? 120 : std::min<uint64_t>(opts.outFrame, 10000));
    uint32_t frames = 0;
    // PNG 序列：直接写目标目录（文件名按顺序 0000.png 递增）
    if (opts.codec == Codec::PngSequence) {
        return scene->exportAnimationToPng(
            w, h, opts.outPath, /*instanceIndex=*/0, opts.inFrame, opts.outFrame,
            opts.step, /*bgOverride=*/nullptr, opts.ssaa, opts.colorDepth, &frames,
            opts.source, orbitFrames);
    }

    // 视频/动图：先写临时帧目录，再交给 ffmpeg 封装；成功后清掉帧目录。
    const std::string framesDir = opts.outPath + ".frames";
    if (!scene->exportAnimationToPng(w, h, framesDir, /*instanceIndex=*/0, opts.inFrame,
                                     opts.outFrame, opts.step, /*bgOverride=*/nullptr,
                                     opts.ssaa, opts.colorDepth, &frames, opts.source,
                                     orbitFrames)) {
        return false;
    }
    engine::AnimEncodeOptions enc;
    enc.framePattern = framesDir + "/frame_%04d.png";
    enc.outPath = opts.outPath;
    switch (opts.codec) {
    case Codec::Apng: enc.format = engine::AnimFormat::Apng; break;
    case Codec::Gif: enc.format = engine::AnimFormat::Gif; break;
    case Codec::Webm: enc.format = engine::AnimFormat::Webm; break;
    case Codec::Mp4:
    case Codec::PngSequence: enc.format = engine::AnimFormat::Mp4; break;
    }
    enc.fps = std::max(1, static_cast<int>(std::lround(opts.fps)));
    enc.loop = opts.loop;
    // 输出路径没有扩展名时按格式补一个，避免 ffmpeg 无法推断容器
    if (std::filesystem::path(enc.outPath).extension().empty()) {
        enc.outPath += engine::animFormatExtension(enc.format);
    }
    std::string err;
    if (!engine::encodeAnimation(enc, &err)) {
        spdlog::error("[io] export(anim): 编码失败: {}", err);
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(framesDir, ec);
    spdlog::info("[io] export(anim): {} 帧 -> {}（{}，{} fps）", frames, enc.outPath,
                 engine::animFormatName(enc.format), enc.fps);
    return true;
}

// QS.4：图像导出放入后台线程执行（不阻塞主视图）。
// GPU 队列提交经 VulkanDevice::lockQueue 与主线程视口串行（见 device.h）；
// 导出线程不触碰 ImGui；完成后写 exportMsg_ 供 UI 展示。
// 动画导出：**主线程同步**执行（不进后台线程）。
// 逐帧导出会改写场景坐标段并重建命令流，与主线程渲染循环共用同一场景/渲染器，
// 放后台线程会与渲染循环数据竞争（图像导出之所以能放后台，是因为它只读命令流）。
// 代价是导出期间界面无响应——这是正确性优先的取舍。
void ExportImgTool::startAnimationExport(const AnimationExportOptions& opts, MolScene* scene) {
    exportRunning_ = true;
    exportFinished_ = false;
    exportOk_ = false;
    {
        std::lock_guard<std::mutex> lk(msgMutex_);
        exportMsg_ = tr("动画导出中（界面暂时无响应）…");
    }
    const bool ok = ExportImgTool::exportAnimation(opts, scene);
    {
        std::lock_guard<std::mutex> lk(msgMutex_);
        exportMsg_ =
            ok ? (tr("动画导出完成: ") + opts.outPath)
               : (tr("动画导出失败: ") + opts.outPath +
                  tr("（请检查目标目录是否可写；ffmpeg 编解码器需在 PATH 中）"));
    }
    exportOk_ = ok;
    exportRunning_ = false;
    exportFinished_ = true;
}

void ExportImgTool::startExport(const ImageExportOptions& opts, MolScene* scene) {
    if (exportThread_.joinable()) {
        exportThread_.join();
    }
    exportRunning_ = true;
    exportFinished_ = false;
    exportOk_ = false;
    {
        std::lock_guard<std::mutex> lk(msgMutex_);
        exportMsg_.clear();
    }
    exportThread_ = std::thread([this, opts, scene]() {
        const bool ok = ExportImgTool::exportImage(opts, scene);
        {
            std::lock_guard<std::mutex> lk(msgMutex_);
            exportMsg_ =
                ok ? (tr("导出完成: ") + opts.outPath)
                   : (tr("导出失败: ") + opts.outPath +
                      tr("（请检查目标目录是否可写；建议保存到“图片”文件夹）"));
        }
        exportOk_ = ok;
        exportRunning_ = false;
        exportFinished_ = true;
    });
}

} // namespace moleculens::app
