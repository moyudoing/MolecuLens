// =============================================================================
// MolecuLens · persist · settings/settings.cpp —— 设置读写实现
// 规格来源：主方案 §12.2（AppData / ~/.config 定位；JSON 序列化）
// =============================================================================

#include "persist/settings/settings.h"

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
        if (t.contains("speed")) timelineSpeed = t["speed"].get<float>();
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
