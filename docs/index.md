# MolecuLens（分子视镜）

**面向结构生物学与计算化学的分子可视化与渲染引擎。**
Load, inspect, style, animate and export in one window — then reproduce the exact same
picture from the command line for a paper.

![Aurora 工作台：导航栏、视图、检查器与状态栏](figs_v2/overview.png)

!!! note "版本 v2.13"
    当前文档集对应 **v2.13（2026-10-22）**，源码 Apache-2.0，Windows / Linux / macOS
    三平台 CMake 构建。长篇文档为中文，代码与注释中英混排。

## 它能做什么

- **Vulkan 实时视口** —— 固定五区工作台的单窗口布局，实时视口带离屏 FBO 路径；
  表面网格在后台线程重建时交互不卡顿，轨迹播放稳定在 98–119 FPS。
- **17 种表示** —— 线框、球棍、空间填充、管状、卡通、表面、QuickSurf、ESP 表面、
  平板、菱形、点云、多面体等；23 套材质预设，CPK / 链 / 残基 / B-factor 多套配色。
- **轨迹即时间轴** —— DCD、XTC、TRR（GROMACS 旧版与 2023+ 布局）逐帧播放，
  逐帧快速表面走 24 帧 LRU 缓存 + 低优先级预取，暂停/跳帧都停在正确的那一帧。
- **内置 CPU 光追** —— 紧凑 BVH 覆盖球、胶囊与三角面：480×360 / 1 spp 从 31.9 s
  降到 138 ms（231×），1080p / 4 spp 约 3.8 s，不需要 GPU 或专用光追硬件。
- **命令行可复现** —— 界面上每一个可见参数都有对应的 CLI 开关，`info` / `render` /
  `convert` 三个子命令即可把论文插图固化成脚本。

## 文档

| 文档 | 内容 |
| --- | --- |
| [软件架构书](01-软件架构书.md) | 分层与依赖方向、模块边界、命令流、渲染管线、ADR 与偏差记录 |
| [功能介绍书](02-功能介绍书.md) | 能力矩阵、表示与材质逐项说明、已知限制 |
| [使用说明](03-使用说明.md) | 安装启动、界面导览、逐模块操作、快捷键、CLI 参数参考、FAQ |
| [测试报告](04-测试报告.md) | 6,452 原子 GROMACS 算例上的表面/导出/轨迹实测与缺陷记录 |
| [DFT 绘图指南](05-DFT常用绘图指南.md) | 论文插图类型 → 数据来源 → 参数 → 工具与支持状态 |
| [开源数据集与演示](06-开源数据集与演示.md) | 可直接下载的公开结构与轨迹、许可与出处、复现命令 |
| [更新记录](_internal/CHANGELOG-2026-10-22-v2.13.md) | 每一版做了什么、为什么、怎么验证 |

## 快速开始

```bash
# 1) 配置与构建（需要 CMake >= 3.21、C++20 编译器、Vulkan SDK 与 vcpkg）
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release -j

# 2) 打开界面（也可以 Ctrl+O 选文件；直接传结构则载入即用）
./build/src/app/Release/moleculens_app data/EPI.pdb

# 3) 无头出图：界面上调好的样式，命令行一字不差地重放
./build/src/cli/Release/moleculens-cli render data/md.gro --traj data/md.xtc \
    --frame 500 --rep ballstick --style publication --bg white \
    --size 1920 --height 1080 --out frame500.png
```

跑一遍测试：

```bash
ctest --test-dir build -C Release --output-on-failure
```

## 数据与演示

仓库自带一组可直接上手的小样本：`data/EPI.pdb`（表儿茶素）、`data/glycine.pdb`、
`data/md.gro` + `data/md.xtc`（2,042 个水分子的 GROMACS 轨迹）。
公开数据集的出处、许可以及复现命令，见 [开源数据集与演示](06-开源数据集与演示.md)。

## 许可与致谢

源码以 **Apache-2.0** 发布；第三方库、字体与测试数据的许可见
[THIRD_PARTY_NOTICES.md](https://github.com/moyudoing/MolecuLens/blob/master/THIRD_PARTY_NOTICES.md)。
*MolecuLens* 与 *分子视镜* 为项目商标，Apache-2.0 不授予商标使用权。
