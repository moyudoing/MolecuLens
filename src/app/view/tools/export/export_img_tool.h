// =============================================================================
// MolecuLens · app · view/tools/export/export_img_tool.h —— 图像导出面板
// 规格来源：主方案 §7.9/§12.5/§12.6（高 DPI：sizeMode/背景/8-16bit/MSAA/fit/
//           DPI 元数据；动画页签：源/帧范围/fps/编解码器；M4 完整落地）
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "app/view/tool/base_tool_view.h"

namespace moleculens::app {

class ExportImgTool : public BaseToolView {
public:
    ~ExportImgTool() override;   // 后台导出线程 join（避免析构悬垂）
    const char* getTitle() const override { return "图像导出"; }
    // 窗口稳定 ID（与 dock_layout.h 的 panel::kExport 必须一致；
    // 它参与 Dock 布局持久化，**不可随语言变化**）
    [[nodiscard]] const char* getStableId() const override { return dock::panel::kExport; }
    void render(DataManager* dm, MolScene* scene) override;

    // 导出参数模型（主方案 §12.5 ImageExportOptions）
    enum class SizeMode { ViewportScale, Absolute };
    enum class Bg { Current, White, Transparent };
    enum class Fit { Contain, Stretch, Crop };
    enum class Codec { PngSequence, Apng, Gif, Mp4, Webm };

    struct ImageExportOptions {
        SizeMode sizeMode = SizeMode::ViewportScale;
        float scale = 2.0f;
        uint32_t width = 1920, height = 1080;
        Bg background = Bg::Current;
        int colorDepth = 8;
        bool srgb = true;
        int msaa = 4;
        Fit fit = Fit::Contain;
        uint32_t dpi = 300;
        std::string outPath;
        // —— CPU 光线追踪（一.7）——
        bool rayTraced = false;    // 渲染方式：false=光栅化 / true=光线追踪
        int rtSamples = 4;         // RT 每像素子采样数（1-8）
        // —— 抗锯齿（QS.3：光栅化用 SSAA 超采样降采样；RT 用 rtSamples）——
        // 默认 2x（不是 1x）：1x 等于没有抗锯齿，导出图在斜边上全是阶梯。
        // 不默认到 4x 是因为内存上限（mol_scene 的 2 GiB 防护）：8K(7680×4320)
        // 在 2x 下已占 530 MiB 离屏缓冲，4x 会直接撞上限而导出失败 ——
        // 小画布可以调到 4/8，大画布保持 2。
        int ssaa = 2;              // 超采样倍数（1 无 / 2 / 4 … 8）
    };
    struct AnimationExportOptions {
        int source = 0;          // 0 轨迹 / 1 相机旋转 / 2 组合
        uint64_t inFrame = 0, outFrame = 0;
        uint32_t step = 1;
        float fps = 30.0f;
        uint32_t scale = 1, msaa = 4;
        // 抗锯齿/位深与图像导出同义（此前动画路径硬编码 ssaa=1、8-bit，
        // 面板上根本没有这两个参数 → 逐帧导出恒定无抗锯齿）
        int ssaa = 2;
        int colorDepth = 8;
        Codec codec = Codec::PngSequence;
        bool loop = true;
        std::string outPath;
    };

    // M4 落地：导出执行
    static bool exportImage(const ImageExportOptions&, MolScene* scene);
    static bool exportAnimation(const AnimationExportOptions&, MolScene* scene);

private:
    void startExport(const ImageExportOptions& opts, MolScene* scene);
    void startAnimationExport(const AnimationExportOptions& opts, MolScene* scene);

    std::thread exportThread_;
    std::atomic<bool> exportRunning_{false};
    std::atomic<bool> exportFinished_{false};
    std::atomic<bool> exportOk_{false};
    std::mutex msgMutex_;
    std::string exportMsg_;

    // —— 面板状态 ——
    // 必须是**成员**而非函数内 static：static 让"面板状态"与"工具实例"生命周期脱钩，
    // 且 UI 局部值与真正生效的 opts 分成两份、只在点按钮时才同步 —— 由此反复出过
    // "界面显示与导出参数不一致"的 bug（见 moleculens-fixes）。改成成员后，
    // 所有 UI 控件直接绑定 opts 字段，不存在第二份状态。
    ImageExportOptions imgOpts_{};
    AnimationExportOptions animOpts_{};
    int sizeModeIdx_ = 0;           // 尺寸模式下拉下标
    int renderModeIdx_ = 0;         // 渲染方式下拉下标
    int presetIdx_ = 2;             // 预设分辨率下标（默认 4K）
    char fileName_[256] = "image.png";
    char animPath_[512] = "export/anim";
};

} // namespace moleculens::app
