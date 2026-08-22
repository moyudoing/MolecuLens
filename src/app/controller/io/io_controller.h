// =============================================================================
// MolecuLens · app · controller/io/io_controller.h —— 文件 IO 控制器
// 规格来源：主方案 §5.8.1（打开/保存/导入导出：ParserRegistry → SceneStructure
//           → 事件 SceneOpened + MoleculeChanged；加载遮罩状态机驱动）
// =============================================================================

#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/rep/representation.h"

namespace moleculens::app {

// 加载进度回调（UI 遮罩；主方案 §7.5）
struct LoadProgress {
    int stage = 0;          // 0 排队 / 1 读文件 / 2 分析键 / 3 上传 GPU / 4 完成 / 5 失败
    float progress = -1.0f; // <0 不确定
    std::string fileName;
};

class IoController : public Subscriber {
public:
    explicit IoController(AppServices& svc);

    // 打开文件（同步解析 + 装配；主方案 §5.8.1 全流程）
    bool openFile(const std::filesystem::path& path);
    // 保存 .mlscene（M4）
    bool saveScene(const std::filesystem::path& path);
    // 截图（M4）
    bool exportPng(const std::filesystem::path& path, int scale);

    void setLoadCallback(std::function<void(const LoadProgress&)> cb) { loadCb_ = cb; }
    void onEvent(const Event&) override;

private:
    std::vector<engine::Representation> repList();
    AppServices& svc_;
    std::function<void(const LoadProgress&)> loadCb_;
};

} // namespace moleculens::app
