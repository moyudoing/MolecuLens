# MolecuLens · 分子视镜

![MolecuLens 字标](docs/figs_v2/wordmark.png)

跨平台（Windows / Linux / macOS）的分子结构与轨迹实时显示引擎，以现代化单窗口桌面应用交付。
v2（Aurora）起，界面重构为**固定五区的模块化工作台**：标题栏 / 菜单 / 文件栏 / 左导航 + 视口 + 检查器 + 快捷条 / 状态栏。
能力覆盖多格式载入、多表示渲染、轨迹播放（DCD / XTC / TRR）、ESP 映射表面、脚本化与插件化。

> 架构与开发依据：`docs/_internal/blueprint.md`（v2 设计基线）+ [01-软件架构书](docs/01-软件架构书.md)（以已落地实现为准）。

## 快速开始

```powershell
# 前置：CMake ≥ 3.21、vcpkg、Vulkan SDK（见 DEPENDENCIES.md）
cd F:\MolecuLens
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_TOOLCHAIN_FILE=F:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug -j
.\build\src\app\Debug\moleculens_app.exe data\EPI.pdb   # GUI（可传结构文件；不传则空白，Ctrl+O 打开）
.\build\src\cli\Debug\moleculens-cli.exe info data\EPI.pdb
# 轨迹帧出图（DCD / XTC / TRR 任一）：
# .\build\src\cli\Release\moleculens-cli.exe render md.gro --traj md.trr --frame 500 --out f500.png
# ESP 映射表面出图（density cube 为输入结构，--esp 为静电势 cube）：
# .\build\src\cli\Debug\moleculens-cli.exe render density.cube --esp ESP.cube --bg white --out esp.png
ctest --test-dir build                 # 单元测试
```

## 功能

- **多格式**：PDB / GRO / SDF / Gaussian cube（`.cube/.cub`，用于 ESP 表面）；轨迹 DCD / XTC / TRR（GROMACS 经典与 2023+ 布局）
- **多表示（16 种）**：线状 / 棍状 / 球棍（默认）/ 空间填充 / 范德华球 / 卡通 / 改进卡通 / 丝带 / 管状 / 甘草棍 /
  表面 / QuickSurf / 平板（辅基）/ 菱形 / 点云 / **ESP 映射表面**
  （电子密度等值面 + 静电势逐顶点着色 + 色标条 + 极值数值标注，见 [ESP_SURFACE.md](ESP_SURFACE.md)）
- **多输出**：实时窗口渲染、离屏视口（FBO + ImGui 贴图）、CLI 无头出图（M4）、
  OBJ / POV / STL / .mlscene 导出
- **轨迹动画导出**：GUI「动画导出」与 CLI `--anim` 支持 PNG 序列 / APNG / GIF / MP4 / WebM
  （后四者经 ffmpeg 封装；可用 `MOLECULENS_FFMPEG` 指定编码器路径）
- **现代化工作台 UI（v2 Aurora）**：固定五区布局、9 个检查器模块（显示/分子/ESP/轨道/轨迹/标注/分析/日志/设置）、
  矢量图标左导航、取色器与材质微调、浅色/深色双主题、中英双语、加载遮罩与快捷键
- **渲染风格（v2.2）**：环绕柔光 + `fwidth` 像素轮廓 + 背景色距离淡出；四套预设（默认/柔和/出版/鲜明），
  出版预设一键白底+关阴影+强轮廓，GUI「设置 → 渲染风格」与 CLI `--style` 共用
- **时间轴与轨迹**（M3 / v2.12）：时间驱动播放、选区循环、帧增量上传；播放帧率 12–60 FPS 可调，
  导入轨迹后拖拽/滚轮缩放正常，时间轴可拖动跳帧
- **视口叠加（v2.12）**：场景包围盒尺寸、比例尺、坐标轴方向（三个开关可分别关闭）
- **工程完备**：Catch2 测试（最近记录 184 例 / 193,765 断言，34 文件）、spdlog 分级日志、目录粒度检查、三平台构建

## 项目结构

> 字体：`data/font/NotoSansSC-Medium-subset.ttf`（Noto Sans SC 子集，SIL OFL 1.1，见
> `data/font/OFL.txt`；生成脚本 `tools/subset_font.py`）。CMake 构建时自动拷贝到可执行目录
> `font/`；缺失时运行期回退系统字体（微软雅黑等）。

```
CMakeLists.txt / vcpkg.json / cmake/ / shaders/   # 构建与着色器（SPIR-V 内嵌）
src/entity/     # 领域模型（元素表/结构识别码/场景/选择语言/键搜索）+ IO + 持久化
src/engine/     # 场景图/表示/命令流/相机/拾取/Renderer + Vulkan 单后端 + 导出器
src/app/        # 组合根/事件总线/8 控制器/工作台（workbench：style · modules · 五区布局）
src/cli/        # 无头 CLI（render/convert/info）
tests/          # 单元/集成测试（element/identity/selection/serializer/camera/cartoon/
                #   scheduler/vulkan smoke/layout/trajectory dcd-xtc-trr）
plugins/ tools/ # 插件 ABI 样例 / RPC 客户端脚本
data/           # 演示夹具（demo.pdb）、字体
```

## 里程碑

| 里程碑 | 交付 |
|---|---|
| M0 骨架 + UI 外壳 | 三平台编译；单窗口 + 自绘标题栏 + Dock + 日志面板；Vulkan 清屏 |
| M1 Core + 解析 + 基础渲染 | 数据模型/StructureKey/PDB·GRO/球棍·空间填充/轨道相机/命令流 + Vulkan 全链 |
| M2 并行 prepare + 表示系统 + 拾取 | 并行生成/表示三元组/离屏 ID 拾取/MolList·Styles·Material |
| M3 时间轴 + 轨迹 | 轨迹/播放状态机/TimeAxis/帧增量上传 |
| M4 Python + RPC + CLI + 导出 | pybind11/控制台/RPC/CLI/OBJ·POV 导出/PNG 截图 |
| M5 插件体系 + 打磨 | 插件 ABI/主题/布局持久化/打包 |
| **v2.0 Aurora** | 界面重构为固定五区工作台；9 个检查器模块；Dock 自由浮动下线（`#if 0` 保留对照） |
| **v2.1 迭代** | 左栏矢量图标、检查器收窄、设置页五卡片、背景取色弹窗、材质微调、Surface 网格参数修复、TRR 轨迹原生读取、CLI `--surf-iso/--surf-grid/--traj/--frame` |
| **v2.2–v2.5** | 渲染风格四预设（默认/柔和/出版/鲜明）与 CLI `--style`；标注模块（随 PNG 与 `.mlscene`）；剖切面、主题强调色与调色盘 |
| **v2.6** | 玻璃观感（视相关玻璃壳 + 边缘吸收/掠射高光）；CLI `--material` 与 `--rep` 五段规格；「一键风格」VMD 玻璃/不透明 |
| **v2.7–v2.8** | 表面并入显示、视口下方时间轴条、检查器 420 px 可拖动、浅色主题白底；UI 组件 P0（`segmented`/`sliderNum`/`stepper`/`badge`/`statChip`） |
| **v2.9** | 表示表格（样式/配色/选择直改、多选批量、Del 删除）、视口分子名标注、分子列表按结构聚合 |
| **v2.10** | 「分子」独立成模块、列表改「体系 → 结构」两级、不同导入沿 +X 并排 |
| **v2.11** | 轨迹面板重构（文件/体系/卸载）、时间轴两行布局、统一 1 基帧号 |

## 开发约定

- 命名：类 `PascalCase`、方法 `snake_case`、成员 `lowercase_`、命名空间 `moleculens`
- 目录粒度：每目录 `.cpp/.h ≤ 3`（`cmake/check_tree.py` 检查）
- 依赖方向：`moleculens_entity ← moleculens_engine ← app/cli`
- 协议头独立编译：`engine/cmd/cmd.h`（附录 F）、`persist/mlscene/mlscene.h`（附录 G）
- 缓存纪律：重建命令流必须 `bumpSerial()`（P5）

## 文档

| 文档 | 内容 |
| --- | --- |
| [01-软件架构书](docs/01-软件架构书.md) | 分层、模块、数据流、命令流、渲染管线、ADR 与设计偏差记录 |
| [02-功能介绍书](docs/02-功能介绍书.md) | 功能矩阵、模块逐项说明、与 VMD/Multiwfn/GXNU 的对照 |
| [03-使用说明](docs/03-使用说明.md) | 安装、界面导览、逐模块操作、快捷键、命令行参数、FAQ |
| [04-测试报告](docs/04-测试报告.md) | GROMACS 6452 原子真实算例：表面/QuickSurf 速度、导出、轨迹、缺陷与修复 |
| [05-DFT常用绘图指南](docs/05-DFT常用绘图指南.md) | 论文插图类型 → 数据来源 → 参数 → 工具 → 本项支持度对照 |
| [ESP_SURFACE.md](ESP_SURFACE.md) | ESP 映射表面的数据链、参数与出图配方 |
| [REMAINING_WORK.md](REMAINING_WORK.md) | 剩余工作索引、i18n 收尾、**环境与构建约定**（新会话先读） |
| [blueprint.md](docs/_internal/blueprint.md) | v2 Aurora 设计基线：分层、五区布局、模块契约 |
| [ISSUES-已知问题登记](docs/_internal/ISSUES-已知问题登记.md) | 未解决缺陷（S2/S3）、建议修复顺序 |
| [ROADMAP-渲染与DFT视图](docs/_internal/ROADMAP-渲染与DFT视图.md) | 渲染/光追/DFT 未落地项与验收标准 |
| [PLAN-UI组件丰富化](docs/_internal/PLAN-UI组件丰富化.md) | UI 设计 token 与待做组件 |
| [REFACTOR-2026-10-22](docs/_internal/REFACTOR-2026-10-22.md) | 结构性问题 + 50 项领域改进清单（渲染/轨迹/交互/测试/构建/文档/合规）与分阶段方案 |
| [archive](docs/_internal/archive/README.md) | 已完成方案与过期交接（仅追溯，含"不要重做"清单） |

对应的 `.docx` 版本由 `tools/md_to_docx.py` 从 Markdown 生成，与 Markdown 内容一致。

## 依赖

全部第三方依赖的安装方式 / 版本要求 / 环境配置见 **[DEPENDENCIES.md](DEPENDENCIES.md)**。

## 许可证

本项目（代码与文档）采用 **Apache License 2.0** 授权，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
第三方依赖、字体与测试数据的来源和许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

- 界面字体 Noto Sans SC 为 SIL OFL 1.1（`data/font/OFL.txt`），随二进制分发时保留该许可。
- `data/traj_xtc_10f10a.xtc` 为 MDAnalysis 的 **GPL-2.0+** 测试夹具，单独声明，不适用 Apache-2.0。
- 项目名称 `MolecuLens` / `分子视镜` 的商标权保留；Apache-2.0 不授予商标许可。
- 欢迎以 issue / PR 参与；提交即表示同意按 Apache-2.0 授权你的贡献（DCO 风格）。
