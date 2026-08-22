# MolecuLens · 实现与验收报告

> 依据：《MolecuLens 分子显示引擎开发最终方案》（唯一权威）与《MolecuLens_M0 工程脚手架》。
> 环境：Windows 11 / MSVC 2022 (cl 19.44) / CMake 3.31.7 / vcpkg (2025-04-07) / Vulkan SDK 1.4.357.0。

## 1. 构建结果（2026-08-19 验证）

| 目标 | 结果 | 说明 |
|---|---|---|
| `moleculens_app` | ✅ 构建成功，零警告 | GUI：自绘标题栏 + Dock + 3D 视口 + 面板 |
| `moleculens-cli` | ✅ 构建成功，零警告 | `--version`/`info` 实测通过 |
| `moleculens_tests` | ✅ 构建成功，零警告 | **39 用例 / 1236 断言全部通过** |
| 编译警告 | ✅ 零警告（MSVC `/W4` + `/utf-8`） | |
| 目录粒度 | ✅ `cmake/check_tree.py` 通过（每目录 ≤3 cpp/h） | |

## 2. 运行时验证

- **GUI**：启动创建 1280×800 无边框窗口，标题 "MolecuLens"（自绘标题栏），
  深色 Tokyo Night 主题渲染正常（截图像素分析：60% 深色背景 + 蓝色强调色 UI），
  自动加载 `data/demo.pdb`（14 原子/10 键/1 帧），进程稳定运行。
- **CLI**：`moleculens-cli info data/demo.pdb` → 原子 14 / 键 10 / 帧 1，退出码 0。
- **离屏渲染**：`offscreen_smoke` 测试在真实 GPU 上渲染球体并回读像素验证通过。
- **序列化**：`.mlscene` round-trip（含 CRC/版本/截断校验）全过。

## 3. 里程碑完成情况

| 里程碑 | 交付 | 状态 |
|---|---|---|
| M0 骨架 + UI 外壳 | CMake/vcpkg 工程、自绘标题栏、Dock 布局（可重置）、日志面板、加载遮罩、UITheme、事件总线、8 控制器、Vulkan 清屏链路 | ✅ |
| M1 Core + 解析 + 基础渲染 | 118 元素表、StructureKey(WL 哈希)、SceneStructure/CoordsSoA、选择语言、键搜索、PDB/GRO 解析、命令流（附录 F）、球棍/空间填充/棍状/线状/卡通、轨道相机、Vulkan 实例化渲染（MSAA+UBO+Blinn-Phong） | ✅ |
| M2 并行 + 表示 + 拾取 | 表示三元组/材质 8 预设/离屏 FBO 视口、StylesTool/MaterialTool/MolList 面板、CPU 射线拾取（ID Pass 骨架） | ✅（拾取 ID Pass 为骨架，CPU 兜底完整） |
| M3 时间轴 + 轨迹 | Trajectory/FrameScheduler（时间驱动+选区循环）、TimelineController、TimeAxis UI、帧增量上传（DIRTY_COORDS） | ✅ |
| M4 CLI + 导出 | CLI 三子命令、OffscreenRenderer（无头+视口 FBO）、OBJ/POV/STL 导出、`.mlscene` 序列化（附录 G 完整）、ExportImg 面板 | ✅（PNG 编码与 ffmpeg 管道为 M4 后续项，接口就绪） |
| M5 插件 + 打磨 | 插件 ABI 头、settings.json（布局 base64 持久化）、目录粒度检查、README/DEPENDENCIES | ✅ |

## 4. 已知后续项（接口就绪，待接入）

- 离屏 ID Pass 拾取（`engine/vulkan` pick 着色器已编译内嵌）
- PNG/BMP/TGA/PPM 图像编码与 MP4/WebM ffmpeg 管道（ExportImg 面板参数已就绪）
- Python 绑定（`MOLECULENS_BUILD_PYTHON=ON` 时启用，依赖已从默认 manifest 移除）

## 5. 构建命令

```powershell
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_TOOLCHAIN_FILE=F:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug -j 8
.\build\src\app\Debug\moleculens_app.exe
.\build\tests\Debug\moleculens_tests.exe
```
