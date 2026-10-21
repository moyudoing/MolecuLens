# MolecuLens v2 · Aurora 重构蓝图（内部设计基线）

> 本文件是 2026-10 重构的设计基线，供架构书/功能书/使用说明的撰写者引用。
> 代码事实以 `src/` 为准；本文件描述**目标结构**与**已经落地/规划中**的边界。
> 术语：MolecuLens v2 代号 **Aurora**；产品名仍为 MolecuLens。

---

## 1. 目标与约束

1. **完全重建 UI 层**：放弃 ImGui docking 自由浮动面板，改为 Multiwfn 式固定工作台
   （左导航 + 顶部文件栏 + 中央视口 + 右侧检查器 + 底部快捷条 + 状态栏）。
2. **模块化**：新增 `app/workbench` 表现层与 `IInspectorModule` 模块契约；面板 = 一个
   模块类 + 一行注册。领域/引擎层保持稳定，通过控制器访问。
3. **渲染管线优化**：保留引擎的两阶段（prepare 命令流 / draw 后端执行）与 serial 缓存；
   v2 增加异步表面任务、渲染统计上报、统一几何缓存与显式 pass 顺序说明。
4. **不破坏**：CLI、Catch2 测试、`.mlscene` 持久化、脚本/headless 路径保持可用。
5. 文档交付：软件架构书、功能介绍书、使用说明（中文，Markdown + DOCX）。

## 2. 分层架构（v2）

```
┌─────────────────────────────────────────────────────────────┐
│ app（组合根 + 表现层）                                       │
│  ├─ workbench/   固定工作台：Workbench / IInspectorModule     │
│  ├─ controller/  8 个用例控制器（Scene/Render/Rep/Selection/  │
│  │               Timeline/Camera/Io）                        │
│  ├─ view/        视口 MolScene、主题、旧工具面板（兼容层）     │
│  └─ shell/       Application 组合根、AppServices 服务表       │
├─────────────────────────────────────────────────────────────┤
│ engine（渲染/几何/分析可视化）                                │
│  render(两阶段) · cmd(命令流) · vulkan(设备/交换链/ImGui)     │
│  style(16 种表示) · async(表面任务线程) · export             │
├─────────────────────────────────────────────────────────────┤
│ io（文件格式）  pdb/gro/sdf/cube/dcd/xtc/mlscene/settings     │
├─────────────────────────────────────────────────────────────┤
│ entity（领域模型，无 UI/Vulkan 依赖）                          │
│  SceneStructure · Atom/Bond · Trajectory · Selection · 单位    │
└─────────────────────────────────────────────────────────────┘
```

依赖方向严格单向：app → engine → io → entity；entity 不 include 上层。
`AppServices`（`src/app/shell/application.h`）是组合根注入的唯一服务表。

## 3. 表现层：Aurora 工作台

### 3.1 视觉语言（浅色 "Aurora Lab" 为默认，另提供深色）

- 背景 `#EEF2F7`，面板 `#FFFFFF`，描边 `#DCE3EC`，正文 `#1F2937`，次要 `#64748B`。
- 主色 `#2563EB`（蓝），辅色 `#0EA5E9`（青），成功 `#10B981`，警告 `#F59E0B`，危险 `#EF4444`。
- 圆角 10px（卡片）/ 8px（控件）；卡片投影极轻；标题 15px semibold，正文 13px，注 11px。
- 字体：OPPO_B.ttf（中文），ImGui 三档字号（16/20/13 由 UITheme 管理）。
- 风格关键词：Lab / Instrument / Clean / Light-first，参考 Multiwfn 的"参数密度"与
  GXNU-MolStudio 的浅色科技风 QSS（#E4EAF2 背景、白卡片、#1565C0 标题）。

### 3.2 布局（像素，1280×800 基线）

- 自绘标题栏 36px：应用标识 + 文档名 + 最小化/最大化/关闭。
- 顶栏第二行 40px：菜单（文件/编辑/视图/分析/帮助）+ 语言切换（中/EN）+ 设置齿轮。
- 文件栏 44px：路径输入框（只读显示当前文件）+ 打开/保存/导出 + 最近文件。
- 左导航栏 92px：9 个模块（图标字形 + 中文标签），选中态为蓝色圆角块。
- 中央视口：自适应，圆角卡片，左上浮动工具条（样式预设/取景/截图），右下色标。
- 右检查器 340px（可拖拽 300–420）：模块参数，卡片分区，可滚动。
- 底部快捷条 46px：一键样式（sob-art / IBOview / HoukMol / IQmol）| 清空样式 |
  隐藏氢 | 背景 | 导出 PNG | 重置视角。
- 状态栏 26px：FPS · 原子数 · 实例数 · 当前帧 · 绘制调用/三角面 · 渲染后端。

### 3.3 模块契约

```cpp
class IInspectorModule {
public:
    virtual ~IInspectorModule() = default;
    virtual const char* id() const = 0;        // 稳定 ID（持久化用）
    virtual const char* label() const = 0;     // 导航标签（tr() 包一层）
    virtual const char* glyph() const = 0;     // 短字形/图标
    virtual void render(Workbench& wb) = 0;    // 渲染右侧检查器
    virtual void onActivate(Workbench&) {}     // 进入模块（懒加载/校验）
};
```

模块清单（v2 实际交付）：
| id | 标签 | 功能 |
|---|---|---|
| scene | 场景 | 分子/体系列表、可见性、删除、体系分组、打开/保存 |
| visualize | 可视化 | 表示管理、类型/配色/选择、半径/键半径、材质/透明度、光照/背景/雾、相机 |
| surface | 表面 | SES/QuickSurf 等值面、网格间距、透明度、整体系表面、异步生成状态 |
| esp | 静电势 | density/potential cube、iso、色标、范围、自动/反相、极值色标 |
| orbital | 轨道 | cube 载入、± 相位着色（复用 ESP 映射表面、同一 cube 作密度与势） |
| trajectory | 轨迹 | 播放/帧/速度/循环/In-Out、DCD/XTC 导入 |
| analysis | 分析 | cube 体数据剖析（维度/原点/步长/范围/直方图）；电荷/NBO/IGMH 标注为路线图 |
| log | 日志 | spdlog 环形缓冲实时查看、级别过滤、清空 |
| settings | 设置 | 主题/语言/MSAA/默认配色/重置布局/关于/快捷键 |

### 3.4 交互

- 视口：左键轨道、中键/滚轮 dolly、右键平移（沿用 CameraController 唯一入口）。
- 快捷键：Ctrl+O 打开、Ctrl+S 保存、Ctrl+E 导出、F 取景、R 重置视角、Space 播放/暂停、
  ←/→ 单帧、1–4 样式预设、Ctrl+L 日志、F1 快捷键帮助。
- 所有参数修改 → 控制器 → 事件总线（`RepresentationChanged` / `SceneChanged`）→ 视口脏标记；
  UI 不直接写 SceneStructure/相机。

## 4. 数据流

```
文件 → io::ParserRegistry → entity::SceneStructure（addInstance/dedup）
     → 事件 SceneOpened/MoleculeChanged
     → RepresentationController（表示列表，唯一持有者）→ renderer.bindRepresentations
     → Renderer::prepare（serial 变更才重建）→ SceneCommandList（网格/材质/变换）
     → VulkanRenderer 记录：shadow pass → main pass（不透明→透明）→ ImGui → present
```

- 坐标：`CoordsSoA`（x/y/z 分块，SIMD 友好）；轨迹帧切换只改坐标段 + `DIRTY_COORDS`。
- 表面：`src/engine/async/surface_jobs` 单工作线程 FIFO + 几何指纹去重；主线程
  `consumeSurfaceJobCompletion` 合并；渲染器在任务未完成时保留旧命令流（画面不闪）。
- 相机：`OrbitCamera` 值类型，`CameraController` 是唯一写入口（交互不广播，按帧读真值）。

## 5. 渲染管线

1. **CPU 构建**：表示 → 网格（球/键/卡通/表面）→ 命令（draw + 变换 + 材质）。
2. **命令流缓存**：`SceneCommandList` 按 serial 复用，几何按指纹进缓存；阴影 pass 复用。
3. **GPU**：单命令缓冲录制；阴影 map → MSAA 主 pass → 透明排序 → ImGui overlay → present。
4. **异步表面**：QuickSurf/SES 在后台线程生成，主线程上传；进度在视口与状态栏可见。
5. **导出**：离屏 FBO（1×–4× SSAA）→ 回读 → PNG；CPU 光追路径独立；动画逐帧。
6. **统计**：v2 在 `Renderer` 暴露 drawCalls/triangles/uploadBytes（供状态栏与架构书引用）。

## 6. 质量与构建

- 构建：CMake ≥3.21 + VS2022 + vcpkg；`tools/build_incremental.bat` 增量构建。
  **post-build 的 MSB3073（applocal.ps1）是已知噪声**，不影响 exe；以 `-> .exe` 为准。
- 测试：Catch2（`moleculens_tests`），历史 156/156；`tools/check_tree.py` 目录粒度
  （每目录 ≤3 个 .cpp/.h）；i18n 覆盖率脚本；clang-format。
- 运行时 DLL：fmt/spdlog/glfw3 从 `build/vcpkg_installed/x64-windows*/bin` 拷到 exe 旁。
- 日志：`logs/`；崩溃：`logs/crash.log`（set_terminate + 栈回溯）。

## 7. 文档交付大纲

### 7.1 软件架构书（docs/01-软件架构书.md）
1. 引言（目标/范围/术语/读者） 2. 架构总览（分层图、依赖规则、质量属性）
3. 领域模型（Scene/Atom/Bond/Trajectory/Selection/单位/PBC）
4. 模块设计（entity/io/engine/app 各节：职责、接口、关键算法、错误处理）
5. 渲染管线（两阶段、命令流、缓存、异步、pass 顺序、性能数据表）
6. UI 架构（Aurora 工作台、模块契约、主题系统、i18n、布局持久化）
7. 并发模型（主线程/解析线程/表面线程、锁与所有权）
8. 数据持久化（.mlscene schema、settings.json、版本兼容）
9. 扩展点（新格式/新样式/新模块/新分析） 10. 构建与部署 11. 质量保证与测试
12. 决策记录 ADR（8–12 条，含"为什么放弃 docking""为什么保留两阶段""为什么浅色优先"）
13. 路线图（GPU compute 表面、IGMH/AIM、Python 脚本、插件 ABI）

### 7.2 功能介绍书（docs/02-功能介绍书.md）
- 产品定位与目标用户；能力矩阵（对比 VMD / Multiwfn / GXNU-MolStudio / PyMOL）
- 按模块讲功能（场景/可视化/表面/ESP/轨道/轨迹/分析/日志/设置），每节：做什么、
  怎么用、参数含义、限制；性能与规模数据；已知限制与路线图。

### 7.3 使用说明（docs/03-使用说明.md）
- 安装与启动（依赖、DLL、首次运行）；界面导览（编号图）；5 分钟快速上手；
  典型工作流（载入 PDB→样式→表面→ESP cube→截图导出；轨迹播放；场景保存）；
  快捷键表；参数参考；故障排查（0xC0000135、黑屏、表面不显示、cube 不匹配）；FAQ。

---

## 8. 事实核查清单（写文档前必须确认）

- 表示类型 13 项、配色 5 项、材质预设数量（见 `engine/rep/representation.h`、
  `engine/render/render_parameter.h`）。
- 支持格式：PDB/GRO/SDF/CUBE/DCD/XTC/MLSCENE（`src/io`）。
- ESP 默认 iso、色标枚举、kcal 换算 627.509474（`esp_surface.h`）。
- 测试数量、性能数字以最新实测为准，不抄旧文档。
- UI 截图必须来自本次 v2 构建（`软件注册/figs_v2/`）。
