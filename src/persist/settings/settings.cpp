// =============================================================================
// MolecuLens · persist · settings/settings.cpp —— 设置读写实现
// 规格来源：主方案 §12.2（AppData / ~/.config 定位；JSON 序列化）
// =============================================================================

#include "persist/settings/settings.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "spdlog/spdlog.h"

namespace moleculens::persist {

nlohmann::json Settings::toJson() const {
    nlohmann::json j;
    j["theme"] = theme;
    j["language"] = language;
    j["accent"] = accent;
    // 顶层键（与 theme 同级）：设置面板记住上次的"链接式/嵌入式"选择
    j["sceneSaveMode"] = sceneSaveMode;
    j["layout"] = layout;
    j["render"] = {{"msaa", msaa},
                   {"vsync", vsync},
                   {"colorScheme", colorScheme},
                   {"fov", fov},
                   {"ambient", ambient}};
    j["timeline"] = {{"loop", timelineLoop}, {"dt", timelineDt}, {"speed", timelineSpeed}};
    // 轨迹播放节奏（v2.19 动态 FPS）：顶层键，与 sceneSaveMode 同一取舍
    // —— 键名 = 字段名，手工排查 settings.json 时能直接对上。
    j["playbackMode"] = playbackMode;
    j["playbackFps"] = playbackFps;
    j["playbackSpeed"] = playbackSpeed;
    j["playbackMaxCatchUp"] = playbackMaxCatchUp;
    j["recentFiles"] = recentFiles;
    j["camera"] = {{"target", {cameraTarget[0], cameraTarget[1], cameraTarget[2]}},
                   {"distance", cameraDistance},
                   {"yaw", cameraYaw},
                   {"pitch", cameraPitch}};
    j["rpc"] = {{"enabled", rpcEnabled}, {"port", rpcPort}, {"token", rpcToken}};
    j["python"] = {{"initialScript", pythonInitialScript}};
    return j;
}

void Settings::fromJson(const nlohmann::json& j) {
    if (j.contains("theme")) theme = j["theme"].get<std::string>();
    if (j.contains("language")) language = j["language"].get<std::string>();
    if (j.contains("accent")) accent = j["accent"].get<std::string>();
    if (j.contains("sceneSaveMode")) {
        // 只接受两个稳定键；其他值忽略（保留默认 "linked"），避免坏配置让保存失败
        const auto v = j["sceneSaveMode"].get<std::string>();
        if (v == "linked" || v == "embedded") {
            sceneSaveMode = v;
        }
    }
    if (j.contains("layout")) layout = j["layout"].get<std::string>();
    if (j.contains("render")) {
        const auto& r = j["render"];
        if (r.contains("msaa")) msaa = r["msaa"].get<int>();
        if (r.contains("vsync")) vsync = r["vsync"].get<bool>();
        if (r.contains("colorScheme")) colorScheme = r["colorScheme"].get<std::string>();
        if (r.contains("fov")) fov = r["fov"].get<float>();
        if (r.contains("ambient")) ambient = r["ambient"].get<float>();
    }
    if (j.contains("timeline")) {
        const auto& t = j["timeline"];
        if (t.contains("loop")) timelineLoop = t["loop"].get<bool>();
        if (t.contains("dt")) timelineDt = t["dt"].get<float>();
        if (t.contains("speed")) {
            timelineSpeed = t["speed"].get<float>();
            // 旧配置只有 timeline.speed：作为播放倍速的回退值（下面 playback 段优先）。
            playbackSpeed = std::clamp(timelineSpeed, 0.1f, 4.0f);
        }
    }
    // 轨迹播放节奏（v2.19 动态 FPS）：顶层 playbackMode / playbackFps /
    // playbackSpeed / playbackMaxCatchUp；缺键走默认值。早期内部构建曾写成嵌套
    // playback 对象，这里继续接受（顶层键更具体，最后覆盖）。
    if (j.contains("playback") && j["playback"].is_object()) {
        const auto& p = j["playback"];
        if (p.contains("mode") && p["mode"].is_string()) {
            const auto v = p["mode"].get<std::string>();
            if (v == "dynamic" || v == "fixed" || v == "refresh") {
                playbackMode = v;
            }
        }
        if (p.contains("fps") && p["fps"].is_number_integer()) {
            playbackFps = std::clamp(p["fps"].get<int>(), 0, 240);
        }
        if (p.contains("speed") && p["speed"].is_number()) {
            playbackSpeed = std::clamp(p["speed"].get<float>(), 0.1f, 4.0f);
        }
        if (p.contains("maxCatchUp") && p["maxCatchUp"].is_number_integer()) {
            playbackMaxCatchUp = std::clamp(p["maxCatchUp"].get<int>(), 1, 8);
        }
    }
    if (j.contains("playbackMode") && j["playbackMode"].is_string()) {
        const auto v = j["playbackMode"].get<std::string>();
        if (v == "dynamic" || v == "fixed" || v == "refresh") {
            playbackMode = v;
        }
    }
    if (j.contains("playbackFps") && j["playbackFps"].is_number_integer()) {
        playbackFps = std::clamp(j["playbackFps"].get<int>(), 0, 240);
    }
    if (j.contains("playbackSpeed") && j["playbackSpeed"].is_number()) {
        playbackSpeed = std::clamp(j["playbackSpeed"].get<float>(), 0.1f, 4.0f);
    }
    if (j.contains("playbackMaxCatchUp") && j["playbackMaxCatchUp"].is_number_integer()) {
        playbackMaxCatchUp = std::clamp(j["playbackMaxCatchUp"].get<int>(), 1, 8);
    }
    if (j.contains("recentFiles")) {
        recentFiles = j["recentFiles"].get<std::vector<std::string>>();
    }
    if (j.contains("camera")) {
        const auto& c = j["camera"];
        if (c.contains("target") && c["target"].is_array() && c["target"].size() == 3) {
            for (int i = 0; i < 3; ++i) {
                cameraTarget[static_cast<size_t>(i)] = c["target"][i].get<float>();
            }
        }
        if (c.contains("distance")) cameraDistance = c["distance"].get<float>();
        if (c.contains("yaw")) cameraYaw = c["yaw"].get<float>();
        if (c.contains("pitch")) cameraPitch = c["pitch"].get<float>();
    }
    if (j.contains("rpc")) {
        const auto& r = j["rpc"];
        if (r.contains("enabled")) rpcEnabled = r["enabled"].get<bool>();
        if (r.contains("port")) rpcPort = r["port"].get<int>();
        if (r.contains("token")) rpcToken = r["token"].get<std::string>();
    }
    if (j.contains("python") && j["python"].contains("initialScript")) {
        pythonInitialScript = j["python"]["initialScript"].get<std::string>();
    }
}

std::filesystem::path SettingsStore::settingsPath() {
    // Windows: %APPDATA%/MolecuLens/settings.json；其他: ~/.config/MolecuLens/
    std::filesystem::path dir;
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        dir = std::filesystem::path(appdata) / "MolecuLens";
    } else {
        dir = std::filesystem::temp_directory_path() / "MolecuLens";
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        dir = std::filesystem::path(xdg) / "MolecuLens";
    } else {
        const char* home = std::getenv("HOME");
        dir = std::filesystem::path(home ? home : ".") / ".config" / "MolecuLens";
    }
#endif
    return dir / "settings.json";
}

entity::Result<Settings> SettingsStore::load() {
    const auto path = settingsPath();
    std::ifstream in(path);
    if (!in) {
        return entity::Result<Settings>::ok(Settings{});   // 无配置 → 默认
    }
    try {
        nlohmann::json j;
        in >> j;
        Settings s;
        s.fromJson(j);
        return entity::Result<Settings>::ok(std::move(s));
    } catch (const std::exception& e) {
        spdlog::warn("[persist] settings parse failed: {} (using defaults)", e.what());
        return entity::Result<Settings>::ok(Settings{});
    }
}

entity::Result<void> SettingsStore::save(const Settings& s) {
    const auto path = settingsPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // 原子写：临时文件 + rename
    const auto tmp = path;
    const auto tmpPath = std::filesystem::path(tmp.string() + ".tmp");
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return entity::Result<void>::error(entity::ErrorCode::IoError,
                                               "Cannot write " + tmpPath.string());
        }
        out << s.toJson().dump(2);
        out.flush();
    }
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        // Windows 上 rename 覆盖失败则直接移除重试
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmpPath, path, ec);
    }
    if (ec) {
        return entity::Result<void>::error(entity::ErrorCode::IoError,
                                           "Cannot rename to " + path.string());
    }
    return entity::Result<void>::ok();
}

} // namespace moleculens::persist
