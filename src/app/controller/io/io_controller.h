// =============================================================================
// MolecuLens · app · controller/io/io_controller.h —— 文件 IO 控制器
// 规格来源：主方案 §5.8.1（打开/保存/导入导出：ParserRegistry → SceneStructure
//           → 事件 SceneOpened + MoleculeChanged；加载遮罩状态机驱动）
// =============================================================================

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/rep/representation.h"

namespace moleculens::app {

class RepresentationController;

// 加载进度回调（UI 遮罩；主方案 §7.5）
struct LoadProgress {
    int stage = 0;          // 0 排队 / 1 读文件 / 2 分析键 / 3 上传 GPU / 4 完成 / 5 失败
    float progress = -1.0f; // <0 不确定
    std::string fileName;
};

class IoController : public Subscriber {
public:
    explicit IoController(AppServices& svc);
    ~IoController() override;   // 后台解析线程 join（避免析构悬垂）

    // 打开文件（QS.5：解析放后台线程；主线程 consumeParsed 合并，避免大文件卡 UI）
    // `.mlscene` 走整场景替换路径（同步载入，见 openSceneFile）
    bool openFile(const std::filesystem::path& path);
    // 保存当前场景为 .mlscene（v2：含原子级细节/体系/光照/表示全参数；轨迹为源文件引用）
    bool saveScene(const std::filesystem::path& path);

    // 导入轨迹文件（当前内建 DCD；XTC 待接入）并挂到目标实例。
    // DCD **不含拓扑**，故要求其原子数与目标结构一致（即与已打开的结构文件同源），
    // 否则拒绝并给出可读原因，而不是把坐标错贴到别的结构上。
    bool importTrajectory(const std::filesystem::path& path);

    // 表示控制器注入（载入场景需整体替换表示列表——它是表示的唯一持有者）
    void setRepresentationController(RepresentationController* ctl) { repCtl_ = ctl; }
    // 截图（M4）
    bool exportPng(const std::filesystem::path& path, int scale);

    void setLoadCallback(std::function<void(const LoadProgress&)> cb) { loadCb_ = cb; }
    void onEvent(const Event&) override;

    // 主线程每帧调用：后台解析完成后在此合并进场景（渲染器读场景须在主线程）
    void consumeParsed();

private:
    void mergeParsed(std::shared_ptr<entity::SceneStructure> parsed,
                     const std::string& name);
    // .mlscene 同步载入（整场景替换：clear → applySnapshot → 刷新渲染器）
    bool openSceneFile(const std::filesystem::path& path);

    AppServices& svc_;
    RepresentationController* repCtl_ = nullptr;
    std::function<void(const LoadProgress&)> loadCb_;

    // 当前场景的来源文件（轨迹以"引用 + 指纹"方式写入 .mlscene，不内嵌帧坐标）
    std::string currentSourcePath_;
    uint64_t    currentSourceSize_ = 0;
    int64_t     currentSourceMtime_ = 0;

    // 后台解析 → 主线程合并的交接（线程安全）
    std::mutex parsedMutex_;
    std::shared_ptr<entity::SceneStructure> parsedScene_;
    std::string parsedName_;
    std::string parsedError_;
    std::atomic<bool> parsedReady_{false};
    std::thread parseThread_;
};

} // namespace moleculens::app
