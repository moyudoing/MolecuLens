// =============================================================================
// MolecuLens · engine/export/anim/anim_encoder.cpp
// =============================================================================

#include "engine/export/anim/anim_encoder.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace moleculens::engine {

namespace {

// 命令行参数里含空格/中文路径时都要加引号
std::string quote(const std::string& s) {
    if (s.find_first_of(" \t\"") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

// 运行命令并收集输出（合并 stderr）。返回进程退出码，-1 表示无法启动。
int runAndCapture(const std::string& cmd, std::string* output) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return -1;
    }
    std::string collected;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        collected += buf.data();
        // 防御：ffmpeg 异常刷屏时不要无限吃内存
        if (collected.size() > 64 * 1024) {
            collected = collected.substr(collected.size() - 16 * 1024);
        }
    }
#ifdef _WIN32
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (output != nullptr) {
        *output = collected;
    }
    return rc;
}

} // namespace

AnimFormat animFormatFromPath(const std::string& path) {
    std::string ext;
    const auto pos = path.find_last_of('.');
    if (pos != std::string::npos) {
        ext = path.substr(pos + 1);
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (ext == "gif") {
        return AnimFormat::Gif;
    }
    if (ext == "webm") {
        return AnimFormat::Webm;
    }
    if (ext == "apng" || ext == "png") {
        return AnimFormat::Apng;
    }
    return AnimFormat::Mp4;
}

const char* animFormatExtension(AnimFormat format) {
    switch (format) {
    case AnimFormat::Apng: return ".apng";
    case AnimFormat::Gif: return ".gif";
    case AnimFormat::Webm: return ".webm";
    case AnimFormat::Mp4: return ".mp4";
    }
    return ".mp4";
}

const char* animFormatName(AnimFormat format) {
    switch (format) {
    case AnimFormat::Apng: return "APNG";
    case AnimFormat::Gif: return "GIF";
    case AnimFormat::Webm: return "WebM";
    case AnimFormat::Mp4: return "MP4";
    }
    return "MP4";
}

std::string findFfmpegExecutable() {
    if (const char* env = std::getenv("MOLECULENS_FFMPEG"); env != nullptr && env[0] != '\0') {
        std::error_code ec;
        if (std::filesystem::exists(env, ec)) {
            return env;
        }
    }
#ifdef _WIN32
    std::string out;
    if (runAndCapture("where ffmpeg 2>nul", &out) == 0) {
        const auto nl = out.find_first_of("\r\n");
        std::string first = out.substr(0, nl);
        std::error_code ec;
        if (!first.empty() && std::filesystem::exists(first, ec)) {
            return first;
        }
    }
#else
    std::string out;
    if (runAndCapture("command -v ffmpeg 2>/dev/null", &out) == 0) {
        const auto nl = out.find_first_of("\r\n");
        std::string first = out.substr(0, nl);
        std::error_code ec;
        if (!first.empty() && std::filesystem::exists(first, ec)) {
            return first;
        }
    }
#endif
    std::error_code ec;
    if (std::filesystem::exists("ffmpeg.exe", ec)) {
        return "ffmpeg.exe";
    }
    if (std::filesystem::exists("ffmpeg", ec)) {
        return "ffmpeg";
    }
    return {};
}

bool encodeAnimation(const AnimEncodeOptions& opts, std::string* error) {
    if (opts.framePattern.empty() || opts.outPath.empty()) {
        if (error != nullptr) {
            *error = "framePattern / outPath 为空";
        }
        return false;
    }
    const std::string ffmpeg = findFfmpegExecutable();
    if (ffmpeg.empty()) {
        if (error != nullptr) {
            *error = "未找到 ffmpeg：请把它放进 PATH，或设置环境变量 MOLECULENS_FFMPEG";
        }
        spdlog::error("[anim] 未找到 ffmpeg（PATH / MOLECULENS_FFMPEG）");
        return false;
    }

    const int fps = opts.fps > 0 ? opts.fps : 30;
    std::vector<std::string> args;
    args.push_back("-y");
    args.push_back("-hide_banner");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-framerate");
    args.push_back(std::to_string(fps));
    args.push_back("-start_number");
    args.push_back(std::to_string(opts.startNumber));
    args.push_back("-i");
    args.push_back(opts.framePattern);

    switch (opts.format) {
    case AnimFormat::Mp4:
        // yuv420p 要求偶数宽高：奇数尺寸补一行/一列，避免编码器直接报错
        args.push_back("-vf");
        args.push_back("pad=ceil(iw/2)*2:ceil(ih/2)*2");
        args.push_back("-c:v");
        args.push_back("libx264");
        args.push_back("-preset");
        args.push_back("medium");
        args.push_back("-crf");
        args.push_back("18");
        args.push_back("-pix_fmt");
        args.push_back("yuv420p");
        args.push_back("-movflags");
        args.push_back("+faststart");
        break;
    case AnimFormat::Webm:
        args.push_back("-c:v");
        args.push_back("libvpx-vp9");
        args.push_back("-b:v");
        args.push_back("0");
        args.push_back("-crf");
        args.push_back("32");
        args.push_back("-pix_fmt");
        args.push_back("yuv420p");
        break;
    case AnimFormat::Apng:
        args.push_back("-c:v");
        args.push_back("apng");
        args.push_back("-pix_fmt");
        args.push_back("rgba");
        args.push_back("-plays");
        args.push_back(opts.loop ? "0" : "1");
        break;
    case AnimFormat::Gif:
        // 单次调用完成调色板生成 + 抖动，避免两趟临时文件
        args.push_back("-filter_complex");
        args.push_back("[0:v]split[a][b];[a]palettegen=stats_mode=diff[p];"
                       "[b][p]paletteuse=dither=bayer:bayer_scale=5");
        args.push_back("-loop");
        args.push_back(opts.loop ? "0" : "-1");
        break;
    }
    args.push_back(opts.outPath);

    std::string cmd = quote(ffmpeg);
    for (const auto& a : args) {
        cmd += ' ';
        cmd += a;
    }
    cmd += " 2>&1";

    std::string output;
    const int rc = runAndCapture(cmd, &output);
    if (rc != 0) {
        if (error != nullptr) {
            *error = output.empty() ? ("ffmpeg 退出码 " + std::to_string(rc)) : output;
        }
        spdlog::error("[anim] ffmpeg 编码失败（rc={}）：{}", rc, output);
        return false;
    }
    spdlog::info("[anim] {} 编码完成: {}（{} fps, {} 帧起）", animFormatName(opts.format),
                 opts.outPath, fps, opts.startNumber);
    return true;
}

} // namespace moleculens::engine
