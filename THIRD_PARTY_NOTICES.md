# 第三方声明（Third-Party Notices）

本文件列出 MolecuLens 分发物中包含或依赖的第三方软件、素材与数据的来源与许可。
项目自身代码与文档采用 **Apache License 2.0**（见 [LICENSE](LICENSE)、[NOTICE](NOTICE)）。

> 原则：项目**不包含** GPL / LGPL / AGPL 代码。少数测试夹具为 GPL-2.0+（见 §3），
> 仅作为测试数据存在于 `data/`，不与产品代码链接、不随二进制分发。

---

## 1. 构建依赖（vcpkg manifest）

| 依赖 | 版本锁定 | 许可 | 说明 |
|---|---|---|---|
| GLFW (`glfw3`) | vcpkg baseline | zlib | 窗口/输入 |
| volk | 同上 | MIT | Vulkan 函数加载 |
| Vulkan-Headers | 同上 | Apache-2.0 | Vulkan 头文件（含 Khronos 版权） |
| VulkanMemoryAllocator | 同上 | MIT | GPU 内存分配 |
| GLM | 同上 | MIT | 数学库 |
| Dear ImGui（docking 分支） | 同上 | MIT | 界面；`src/app/imgui/backends/` 为上游 backends 原样副本 |
| spdlog | 同上 | MIT | 日志（其传递依赖 `fmt` 同为 MIT） |
| nlohmann-json | 同上 | MIT | JSON |
| lodepng | 同上 | zlib | PNG 编解码 |
| CLI11 | 同上 | BSD-3-Clause | 命令行解析 |
| Catch2 | 同上 | BSL-1.0 | 仅测试目标 `moleculens_tests` |
| cppzmq | 同上（仅非 Windows） | MIT | 可选 RPC；默认 `MOLECULENS_ENABLE_ZMQ=OFF` |
| libzmq（cppzmq 的运行时） | — | **MPL-2.0** | 仅启用 ZMQ 时链接；文件级回源义务 |

构建期工具（不进入分发物）：`glslangValidator`（Khronos，Apache-2.0/BSD）在 configure
阶段把 GLSL 编译为内嵌 SPIR-V。

可选：`MOLECULENS_QUICKSURF_CUDA=ON` 时链接 CUDA Runtime（NVIDIA 专有 EULA），
默认关闭、分发物中不含 NVIDIA 组件。

## 2. 素材

| 素材 | 许可 | 位置 |
|---|---|---|
| **Noto Sans SC**（子集 + `wght=500` 实例化，Modified Version） | SIL Open Font License 1.1 | `data/font/NotoSansSC-Medium-subset.ttf`，许可全文 `data/font/OFL.txt`；版权行：Copyright © 2014-2021 Adobe, with Reserved Font Name 'Source' |

随二进制分发时必须一并携带 `font/OFL.txt`（CMake 已自动拷贝）。

## 3. 数据与测试夹具

| 文件 | 来源 | 许可 | 处理 |
|---|---|---|---|
| `data/traj_xtc_10f10a.xtc` | MDAnalysis 测试数据 `xtc_test_only_10_frame_10_atoms.xtc` | **GPL-2.0+** | 仅测试夹具，独立声明；**不适用**本项目的 Apache-2.0 |
| `data/md.gro`、`md.xtc`、`EPI.*`、`API.gro`、`glycine.pdb` 等 | 项目自造 / 演示数据 | 随项目（Apache-2.0） | `md.xtc` 为工作区既有真实轨迹，**来源与许可未经核实**（见 `data/README.md`） |

## 4. 仅作参考、未包含其代码

| 项目 | 许可 | 关系 |
|---|---|---|
| **VMD 1.9.4a57** | UIUC 非商用许可 | 仅参考算法思路、材质命名与界面组织；**未复制其代码**（源码相似度核验：同源代码行 ≤1、5-gram 重合 <0.01，材质参数表不同）。VMD 源码与二进制均不包含在本仓库/分发物中 |
| GXNU-MolStudio、Multiwfn | 各自许可 | 仅作界面/交互参考，未包含代码 |
| Tachyon | UIUC | 未使用其代码（本项目光追为自研 BVH） |

## 5. 商标与命名

- `MolecuLens` / `分子视镜` 为项目名称，Apache-2.0 不授予商标权。
- 界面中出现的 `VMD`、`Multiwfn` 等名称仅用于指明对标/参考对象，不表示隶属或官方背书。

---

如发现遗漏或错标，请提交 issue；分发前请以各上游项目 **最新许可原文** 为准。
