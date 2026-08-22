# MolecuLens · 分子视镜

跨平台（Windows / Linux / macOS）的分子结构与轨迹实时显示引擎，以现代化单窗口桌面应用交付。
能力覆盖多格式载入、多表示渲染、轨迹播放、脚本化与插件化。

> 架构与开发依据：《MolecuLens 分子显示引擎开发最终方案》（plan/ 目录，唯一权威依据）。

## 快速开始

```powershell
# 前置：CMake ≥ 3.21、vcpkg、Vulkan SDK（见 DEPENDENCIES.md）
cd F:\MolecuLens
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_TOOLCHAIN_FILE=F:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug -j
.\build\Debug\moleculens_app.exe      # GUI（自动加载 data/demo.pdb）
.\build\Debug\moleculens-cli.exe info data\demo.pdb
ctest --test-dir build                 # 单元测试
```

## 功能

- **多格式**：PDB / GRO（首期）；XYZ / MOL2 / SDF / XTC 架构预留（插件式）
- **多表示**：线状 / 棍状 / 球棍（默认）/ 空间填充 / 卡通 / 表面（预留）/ 标签（预留）
- **多输出**：实时窗口渲染、离屏视口（FBO + ImGui 贴图）、CLI 无头出图（M4）、
  OBJ / POV / STL / .mlscene 导出
- **现代化单窗口 UI**：自绘标题栏、Dock 布局（可重置）、Tokyo Night 设计系统、
  加载遮罩状态机、中文文案
- **时间轴与轨迹**（M3）：时间驱动播放、选区循环、帧增量上传
- **工程完备**：Catch2 测试、spdlog 分级日志、目录粒度检查、三平台构建

## 项目结构

> 字体：`data/font/OPPO B.ttf`（**注意文件名带空格**）。CMake 构建时自动拷贝到
> 可执行目录 `font/OPPO_B.ttf`（下划线名）；缺失时运行期回退系统字体（微软雅黑等）。

```
CMakeLists.txt / vcpkg.json / cmake/ / shaders/   # 构建与着色器（SPIR-V 内嵌）
src/entity/     # 领域模型（元素表/结构识别码/场景/选择语言/键搜索）+ IO + 持久化
src/engine/     # 场景图/表示/命令流/相机/拾取/Renderer + Vulkan 单后端 + 导出器
src/app/        # 组合根/事件总线/8 控制器/视图层（ViewControl→MainView→ToolView）
src/cli/        # 无头 CLI（render/convert/info）
tests/          # 单元/集成测试（element/identity/selection/serializer/camera/
                #   cartoon/scheduler/vulkan smoke/layout）
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

## 开发约定

- 命名：类 `PascalCase`、方法 `snake_case`、成员 `lowercase_`、命名空间 `moleculens`
- 目录粒度：每目录 `.cpp/.h ≤ 3`（`cmake/check_tree.py` 检查）
- 依赖方向：`moleculens_entity ← moleculens_engine ← app/cli`
- 协议头独立编译：`engine/cmd/cmd.h`（附录 F）、`persist/mlscene/mlscene.h`（附录 G）
- 缓存纪律：重建命令流必须 `bumpSerial()`（P5）

## 依赖

全部第三方依赖的安装方式 / 版本要求 / 环境配置见 **[DEPENDENCIES.md](DEPENDENCIES.md)**。
