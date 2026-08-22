// =============================================================================
// MolecuLens · persist · settings/settings.h —— 设置读写（settings.json）
// 规格来源：主方案 §12.2（存于用户目录 AppData/~/.config，经 std::filesystem 定位；
//           布局 base64(imgui_dock_serialized)；最近文件；渲染/时间轴/相机/RPC）
// =============================================================================

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "entity/result.h"

namespace moleculens::persist {

struct Settings {
    std::string theme = "dark";
    std::string accent = "teal";
    std::string layout;                          // base64(imgui_dock_serialized)
    int msaa = 4;
    std::string colorScheme = "ElementCPK";
    float fov = 45.0f;
    float ambient = 0.3f;
    bool timelineLoop = true;
    float timelineDt = 2.0f;
    float timelineSpeed = 1.0f;
    std::vector<std::string> recentFiles;
    std::array<float, 3> cameraTarget{0, 0, 0};
    float cameraDistance = 40.0f;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.5f;
    bool rpcEnabled = false;
    int rpcPort = 5555;
    std::string rpcToken;
    std::string pythonInitialScript;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json&);
};

class SettingsStore {
public:
    // 定位用户配置目录并返回 settings.json 路径
    static std::filesystem::path settingsPath();
    static entity::Result<Settings> load();
    static entity::Result<void> save(const Settings&);
};

} // namespace moleculens::persist
