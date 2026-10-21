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
#include <string>
#include <thread>
#include <vector>

#include "app/events/event_bus.h"
#include "app/shell/application.h"
#include "engine/rep/representation.h"
#include "persist/mlscene/mlscene.h"   // Annotations（标注桥）

namespace moleculens::app {

class RepresentationController;

// 加载进度回调（UI 遮罩；主方案 §7.5）
struct LoadProgress {
    int stage = 0;          // 0 排队 / 1 读文件 / 2 分析键 / 3 上传 GPU / 4 完成 / 5 失败
    float progress = -1.0f; // <0 不确定
    std::string fileName;
};

// 场景保存的轨迹存储方式：
//   Linked   —— 只存轨迹源文件路径 + 指纹（默认；文件小，源文件移动后需重新指定）
//   Embedded —— 把轨迹帧坐标写进 .mlscene（单文件可分发，大体系文件会到数百 MB）
enum class SceneSaveMode { Linked, Embedded };

class IoController : public Subscriber {
public:
    explicit IoController(AppServices& svc);
    ~IoController() override;   // 后台解析线程 join（避免析构悬垂）

    // 打开文件（QS.5：解析放后台线程；主线程 consumeParsed 合并，避免大文件卡 UI）
    // `.mlscene` 走整场景替换路径（同步载入，见 openSceneFile）
    bool openFile(const std::filesystem::path& path);
    // 保存当前场景为 .mlscene（v2：含原子级细节/体系/光照/表示全参数）。
    // mode 默认 Linked → 旧调用点无需改动（行为与之前完全一致）。
    bool saveScene(const std::filesystem::path& path,
                   SceneSaveMode mode = SceneSaveMode::Linked);

    // 模式的稳定字符串键（写 Settings / 日志用；不存枚举序号，增删枚举不漂移）
    static const char* modeKey(SceneSaveMode mode);
    static SceneSaveMode modeFromKey(const std::string& key);

    // 导入轨迹文件（内建 DCD / XTC）并挂到目标实例。
    // 两种格式**都不含拓扑**，故要求其原子数与目标结构一致（即与已打开的结构
    // 文件同源），否则拒绝并给出可读原因，而不是把坐标错贴到别的结构上。
    // 单位差异已由读取器抹平（XTC 文件内是 nm，返回前换算为 Å）。
    bool importTrajectory(const std::filesystem::path& path);

    // 表示控制器注入（载入场景需整体替换表示列表——它是表示的唯一持有者）
    void setRepresentationController(RepresentationController* ctl) { repCtl_ = ctl; }

    // —— 场景标注桥（v2.3）——
    // 保存：把 Workbench 当前标注写进快照；载入：把文件里的标注回填给 Workbench。
    // 为什么不把 Workbench 当依赖：IoController 属于控制器层，Workbench 是表现层，
    // 反向依赖会让控制器无法脱离 UI 复用；这里只注册两个纯数据回调。
    using AnnotationGetter = std::function<persist::mlscene::Annotations()>;
    using AnnotationSetter = std::function<void(const persist::mlscene::Annotations&)>;
    void setAnnotationBridge(AnnotationGetter getter, AnnotationSetter setter) {
        annotationGet_ = std::move(getter);
        annotationSet_ = std::move(setter);
    }
    // 截图（M4）
    bool exportPng(const std::filesystem::path& path, int scale);

    void setLoadCallback(std::function<void(const LoadProgress&)> cb) { loadCb_ = cb; }
    void onEvent(const Event&) override;

    // 当前场景的来源文件路径（空 = 尚未打开任何文件）。
    // 用途：ESP 映射表面参数面板把"当前加载的 cube 文件"作为密度 cube 默认值
    // （打开 density.cube 时场景里的原子就来自它）。
    [[nodiscard]] const std::string& currentSourcePath() const {
        return currentSourcePath_;
    }

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
    AnnotationGetter annotationGet_;
    AnnotationSetter annotationSet_;

    // 当前场景的来源文件（链接式保存时写入轨迹引用 + 指纹；嵌入式写入帧坐标）
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
