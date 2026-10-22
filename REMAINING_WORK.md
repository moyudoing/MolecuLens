# MolecuLens 剩余工作与工程约定

> 快照：2026-10-22 02:05（Asia/Shanghai），代码基线 v2.12。
> 本文**只保留跨文档的开放项索引与环境/构建约定**；具体问题、验收标准、UI 组件分别落在：
> - 未解决缺陷：[`docs/_internal/ISSUES-已知问题登记.md`](docs/_internal/ISSUES-已知问题登记.md)
> - 渲染 / 光追 / DFT 路线：[`docs/_internal/ROADMAP-渲染与DFT视图.md`](docs/_internal/ROADMAP-渲染与DFT视图.md)
> - UI 组件：[`docs/_internal/PLAN-UI组件丰富化.md`](docs/_internal/PLAN-UI组件丰富化.md)
> - 结构改造：[`docs/_internal/REFACTOR-2026-10-22.md`](docs/_internal/REFACTOR-2026-10-22.md)
> - 2026-10-22 之前的已完成工作与实测记录：[`docs/_internal/archive/`](docs/_internal/archive/README.md)
>
> 新会话先读这一节「环境与命令」，否则很容易白跑一轮。

---

## 一、开放项（跨文档索引）

| 项 | 现状 | 去哪看 |
|---|---|---|
| GPU 密度核（surface compute） | `shaders/surface.comp` + `vulkan/compute/surface_compute.h` 已就绪；缺 `.cpp`、着色器未注册进 CMake、仓库内无 `vkCmdDispatch` | ROADMAP §2.7 |
| 自适应 LOD | 未做；细分仍为静态两档（预览 3/24、导出 4/48） | ROADMAP §2.1 |
| DFT 属性视图（MO/能级图、DOS、CDD、自旋…） | 只有 cube → ESP 表面 | ROADMAP §2.2–§2.5 |
| 光追观感（表面不透明度、AO/GGX、降噪） | 未做；BVH 与 RT LOD 已完成 | ROADMAP §2.6、ISSUES I-25 |
| 逐实例 QuickSurf 缓存 | 实现过、因相位差回退，待决策 | ISSUES I-17 |
| i18n 覆盖率 | **约 428 条中文未收录**（2026-10-22 实测：408 条 + 预览 20 条） | 本文 §2 |
| 其它缺陷 | 18 条（S2 6 / S3 12） | ISSUES 清单 |

---

## 二、i18n 覆盖率收尾

`tools/check_i18n_coverage.py` 的"未收录"数就是剩余文案数；当前约 428 条（中文源串为键，中英对照在
`src/app/i18n/language.cpp` 的 `kTable`）。

```powershell
$env:PYTHONIOENCODING="utf-8"
& F:\Python\Python39\python.exe tools\check_i18n_coverage.py --list-missing
# 把 中文<TAB>English 写进一个文本文件（用文件工具写，别用 PowerShell 拼字符串），再导入：
& F:\Python\Python39\python.exe tools\add_i18n_entries.py <你的文件.txt>
```

- 剩余为 0 时工具退出码 0，可挂进 CI。
- 三个坑（见 `src/app/i18n/language.h` 注释）：
  1. 跨行隐式拼接的字面量必须**整体**包一次 `tr()`（`tr(a) tr(b)` 不合法）。
  2. 含 `\0` 的 ImGui 多选项串不能进翻译表（`tr(const char*)` 在首个 NUL 截断），要用 `trItems({...})` 逐项翻译。
  3. PowerShell 里 `` `0 `` 是 NUL、弯引号是字符串定界符 → 词条文件用文件工具生成。

---

## 三、剩余小项

- **阴影的 TriMesh 覆盖**：表面 / 卡通 / QuickSurf 目前不投影（只有球 / 柱投影）。需要重构 `draw_geometry`
  的缓冲上传职责（`shadow_map.h` 头注释有说明）。
- **导入时的一次性卡顿**：`[perf]` 日志里"工作量 max"会出现约 200 ms 尖峰（全量命令流重建 + GPU 上传）。
  可后台重建 + 首帧渐进上传。
- **分子实例级操作**：聚合列表无法单独选中某个实例（ISSUES I-24），与 `listRow` 组件一起做。

---

## 四、环境与命令

- **构建**（`vcvars64.bat` 不在 PATH，必须全路径）：
  ```powershell
  cmd /c "F:\MolecuLens\tools\build_incremental.bat"      # 增量
  cmd /c "F:\MolecuLens\tools\build_rebuild.bat"          # 全量
  ```
  Debug 与 Release 都要跑：`msbuild F:\MolecuLens\build\MolecuLens.sln -t:<target> -p:Configuration=Release -m -v:minimal`
  （⚠ `-t:a -t:b` 要写成两个 `-t:`，用 `;` 会被 cmd 当命令分隔符）。构建前先关掉 `moleculens_app`，否则链接失败。
- **post-build 必然报 `MSB3073`**（vcpkg 的 `applocal.ps1` 在本机起不来）——**不是编译失败**。
  判断依据：日志里出现 `<target>.vcxproj -> ...exe` 产出行。
- **运行期 DLL**：因上一条，`fmt.dll` / `spdlog.dll` / `glfw3.dll` 不会自动拷贝，缺了就 `0xC0000135`。
  从 `build\vcpkg_installed\x64-windows\bin`（Release）或 `...\debug\bin`（Debug）复制到 exe 旁。
- **测试**：`build\tests\<Config>\moleculens_tests.exe`，最近记录 **184 例 / 193,765 断言全过**（2026-10-22 02:36，字体替换后复跑）；
  本轮文档更新没有重新构建，改代码后必须复跑。
- **基准**：`pwsh -File tools\run_bench.ps1 -Config Release` → `other\bench_out\bench_report.md`；
  引擎级基准是 `moleculens_bench`（headless GPU，`--rep/--material/--png/--verbose`）。
- **着色器**：`cmake/shaders.cmake` 在 configure 期编译 `shaders/*` → `build/gen/shaders/*.h`；改着色器后必须
  重新 `cmake -S . -B build`，只跑增量构建不会更新内嵌 SPIR-V。
- **目录粒度规则**：任一目录 `.cpp/.h` ≤ 3 个（`cmake/check_tree.py`，现有 2 处豁免：`src/app/imgui/backends`、`tests/vulkan`）。
- **不要信 `git HEAD`**：本机 PATH 里没有 `git`，工作区有大量 untracked/新增源码，**基线就是工作区本身**。
- **日志**：写在**当前工作目录**的 `logs\moleculens.log`，同一 exe 可能有多份；排查"日志停更"前先确认哪份在涨。
- **GUI 验收**：需要解锁桌面（锁屏下 GLFW 无法建窗）；GUI 授权按 agent 会话隔离，子会话申请会卡审批。

---

## 附：§1.1 引用锚点（已完成，2026-10-18）

> 代码注释里的「REMAINING_WORK §1.1」指的是异步 / 渐进生成规格（`src/engine/async/surface_jobs.*`、
> `Renderer::setAsyncSurfaceEnabled`、`prepareBlocking()`）。该功能已合入，**不再是待办**；
> 实现与实测数字（主线程 207.69 ms → 0.39 ms）见归档
> [`archive/REMAINING_WORK-20261022-全量.md §1.1`](docs/_internal/archive/REMAINING_WORK-20261022-全量.md)。
