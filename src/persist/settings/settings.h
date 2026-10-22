// =============================================================================
// MolecuLens · persist · settings/settings.h —— 设置读写（settings.json）
// 规格来源：主方案 §12.2（存于用户目录 AppData/~/.config，经 std::filesystem 定位；
//           布局 base64(imgui_dock_serialized)；最近文件；渲染/时间轴/相机/RPC）
// =============================================================================

#pragma once

#include <array>
#include <filesystem>   // settingsPath() 返回 std::filesystem::path
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "entity/result.h"

namespace moleculens::persist {

struct Settings {
    // UI 主题预设的**稳定键**（app::UITheme::themeKey()，如 "tokyo-night"）。
    // 存键不存序号：预设增删/重排后旧配置仍然指向同一个主题。
    std::string theme = "tokyo-night";
    // 界面语言：稳定键 "zh" / "en"（与主题同一取舍：存字符串不存枚举序号）。
    // 默认中文 —— 与历史行为一致，旧配置缺该键时也走这里。
    std::string language = "zh";
    std::string accent = "teal";
    // 场景保存的轨迹模式：稳定键 "linked" / "embedded"（UI 记忆上次选择）。
    // 存字符串不存枚举序号 —— 与 theme/language 同一取舍，枚举增删不漂移。
    // 缺键（旧配置）→ "linked"，与历史行为一致。
    std::string sceneSaveMode = "linked";
    std::string layout;                          // base64(imgui_dock_serialized)
    int msaa = 4;
    // 垂直同步：true = FIFO（帧率跟显示器刷新率，画面最稳）；false = IMMEDIATE（不受限，
    // 交互更跟手但可能撕裂）。交换链在启动时创建，改动**下次启动生效**。
    bool vsync = true;
    std::string colorScheme = "ElementCPK";
    float fov = 45.0f;
    float ambient = 0.3f;
    bool timelineLoop = true;
    float timelineDt = 2.0f;
    float timelineSpeed = 1.0f;
    // —— 轨迹播放节奏（v2.19 动态 FPS）——
    // 稳定键 "dynamic" | "fixed" | "refresh"（存字符串不存枚举序号：日后增删模式
    // 不会让旧配置指向错误的节奏；缺键走默认 dynamic）。
    std::string playbackMode = "dynamic";
    int playbackFps = 60;          // 动态的目标上限 / 固定的帧率；0 = 跟随渲染
    float playbackSpeed = 1.0f;    // 播放倍速（0.1–4.0）
    int playbackMaxCatchUp = 2;    // 每渲染帧最多追几帧（1–8）
    std::vector<std::string> recentFiles;
    std::array<float, 3> cameraTarget{0, 0, 0};
    float cameraDistance = 40.0f;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.5f;
    bool rpcEnabled = false;
    int rpcPort = 5555;
    std::string rpcToken;
    std::string pythonInitialScript;

    [[nodiscard]] nlohmann::json toJson() const;
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
