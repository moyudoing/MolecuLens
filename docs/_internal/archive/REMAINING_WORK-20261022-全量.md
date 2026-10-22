# MolecuLens 剩余工作（交接文档）

> 生成于 2026-09-26。已完成部分见文末"已完成"；本文件重点是**尚未做**的部分，
> 含具体文件、做法与验收标准，可直接在新对话里接着做。
> 工作环境与坑见"环境与命令"，新会话务必先读这一节（否则会白跑一轮）。

---

## 一、性能：表面生成的异步化与 GPU 加速（优先级最高）

### 现状（实测数据，Release，`data/md.gro` 6452 原子 / 2042 实例）

| 场景 | 当前耗时 |
|---|---|
| 点 QuickSurf（整体系合并表面） | **197 ms**（阻塞主线程，界面不动一下） |
| 之后拖透明度滑杆 | **32 ms/帧**（缓存命中，可接受） |
| 改网格间距/等值面 | 106 ms（必须重算） |
| 点 QuickSurf（每实例表面，小体系才走） | 846 ms，且命令流 316 MB |

**目标**：点击到出图**无可感知延迟**（< 100 ms，且界面不卡）。

### 1.1 异步 + 渐进生成（✅ 已完成，2026-10-18）

**实现**（原"照抄后台解析线程"的方案已落地，且用单工作线程 + 同键去重实现）：
- 新模块 `src/engine/async/surface_jobs.{h,cpp}`：单工作线程 FIFO；`submitSurfaceJob`
  按几何指纹去重（同键排队/运行中不重复计算）；`consumeSurfaceJobCompletion` 给主线程
  消费完成通知；`waitForSurfaceJobs` 给导出线程等待；`shutdownSurfaceJobs` 幂等退出。
- 样式层 `quicksurf.cpp`：整体系 QuickSurf 缓存未命中时，异步模式下**提交任务并立即
  返回**（本帧不发射该表面）；计算体抽为纯 CPU 的 `buildSystemQuickSurfMesh`（只吃
  数值拷贝，不引用 RepContext/场景/命令流），结果写线程安全的 `g_systemMeshCache`。
- 渲染器 `Renderer`：`setAsyncSurfaceEnabled`（GUI 打开，默认关）；重建时若"本轮有
  后台表面任务"，**丢弃新命令流、保留旧命令流**（旧画面继续显示，不清脏位以保持
  门控打开），任务完成后 `prepare()` 自动重建并命中缓存换入；`prepareBlocking()`
  供导出线程（图像/光追/动画）先等后台结果再同步重建，导出图不会出现旧画面；
  `update/prepare` 用同一把 `prepareMutex_` 串行化主线程与导出线程。
- UI：视口左上角显示「表面生成中…」；GUI 初始化时打开异步（CLI/测试默认同步，
  行为与既有一致）。

**实测（`data/md.gro` 6452 原子 / 2042 实例，`moleculens_bench --rep quicksurf
--system-surface --async-surface`）**：

| 指标 | 同步（旧） | 异步（新） |
|---|---|---|
| 主线程构建命令流 | **207.69 ms**（界面卡住） | **0.39 ms**（avg，max 0.47） |
| 后台生成总时长 | —（就是上面那 207 ms） | 87.8 ms（不占 UI 线程） |
| 命令流结果 | 2 个网格 / 19.8 MB | 1 个网格（同一表面） |

残留：每实例 QuickSurf 路径（小体系才走）仍是同步生成，见 1.3；surface（SES）整体系
路径也可按同样方法接入（当前只接了 QuickSurf）。

旧方案备忘（已过时）：`Renderer::prepare()` 在主线程同步跑
`drawQuickSurf`/`drawSystemQuickSurf`，197 ms 全在 UI 线程。

### 1.2 GPU compute（把 197 ms 再压一个量级）

- `shaders/surface.comp` **已写好**（6486 B），`src/engine/vulkan/compute/surface_compute.h`
  的契约（push constant 128B、SSBO、`compute()` 签名）也已定，
  **缺的是 `surface_compute.cpp`**（该目录目前只有 .h）。
- 需要从零建的 compute 基础设施（仓库内目前 `vkCmdDispatch` 出现次数 = **0**）：
  1. `shaders/CMakeLists.txt` 注册 `surface.comp` → 生成 `gen/shaders/surface_comp.h`
     （用现成的 `moleculens_add_shader`，见 `cmake/shaders.cmake`）。
  2. `vkCreateComputePipelines` + 描述符布局/池 + SSBO + `vkCmdDispatch` + 屏障。
     **参考 `src/engine/vulkan/shadow/shadow_map.cpp`** —— 它就是"自带管线的独立 pass"
     的现成范例（自带 `ShaderModule`/`PipelineBuilder`/renderPass/framebuffer）。
  3. 队列提交必须持 `VulkanDevice::lockQueue()`（`device.h`）。
  4. 失败/不支持 → **回退 CPU**（`quicksurf.h` 已有 `quickSurfSetBackend` 开关）。
- 验收：`--rep quicksurf --system-surface` 的"构建命令流"耗时显著下降；
  与 CPU 路径产出的网格在视觉上一致（可用 `--png` 出图比对）。

### 1.3 per-instance 路径的缓存

`quicksurf.cpp` 的 `g_systemMeshCache` 只服务于整体系路径。小体系（≤32 实例）走
`drawQuickSurf` 每实例路径，拖参数时仍会每次重算。做法与 system 版相同
（键只含几何输入 + 互斥）。收益随实例数增长，中等优先级。

---

## 二、i18n 覆盖率收尾

`tools/check_i18n_coverage.py` 的"未收录"数即剩余文案数。补法：

```powershell
$env:PYTHONIOENCODING="utf-8"
& F:\Python\Python39\python.exe tools\check_i18n_coverage.py --list-missing
# 把 中文<TAB>English 写进一个文本文件（**用 Write 工具写文件**，别用 PowerShell 字符串，
# 见下方"坑"），再导入：
& F:\Python\Python39\python.exe tools\add_i18n_entries.py <你的文件.txt>
```

- 剩余为 0 时工具退出码 0，可挂进 CI。
- ⚠ 三个坑（都已踩过，见 `src/app/i18n/language.h` 注释）：
  1. 跨行隐式拼接的字面量必须**整体**包一次 `tr()`（`tr(a) tr(b)` 不合法）。
  2. 含 `\0` 的 ImGui 多选项串**不能**进翻译表（`tr(const char*)` 在首个 NUL 截断），
     要用 `trItems({...})` 逐项翻译。
  3. PowerShell 里 `` `0 `` 是 NUL、`“”` 是字符串定界符 → 词条文件用 Write 工具生成。

---

## 三、可选：其它已识别但未做的小项

- **阴影的 TriMesh 覆盖**：表面/卡通/QuickSurf 目前不投影（只有球/柱投影）。
  需要重构 `draw_geometry` 的缓冲上传职责（`shadow_map.h` 头注释有说明）。
- **阴影贴图尺寸硬编码 2048**：两个后端各一个常量，与 `LightParams::shadowMapSize`
  必须一致才可做成可调项。
- **限帧的睡眠粒度**：应用实测帧周期 9.4 ms 而实际工作量仅 0.8 ms —— 限帧目标
  120 FPS（8.33 ms）被 `sleep_for` 的粒度拖到 107 FPS。改高精度等待可拿回约 13 FPS。
- **导入时的一次性卡顿**：`[perf]` 日志里"工作量 max"会出现 ~200 ms 尖峰
  （全量命令流重建 + GPU 上传）。可后台重建 + 首帧渐进上传。

---

## 环境与命令（新会话先读）

- **构建**（`vcvars64.bat` 不在 PATH，必须全路径）：
  ```powershell
  cmd /c "F:\MolecuLens\tools\build_incremental.bat"      # 增量
  cmd /c "F:\MolecuLens\tools\build_rebuild.bat"          # 全量
  ```
  Debug 与 Release 都要跑：`msbuild F:\MolecuLens\build\MolecuLens.sln -t:<target> -p:Configuration=Release -m -v:minimal`
  （⚠ `-t:a -t:b` 要写成两个 `-t:`，用 `;` 会被 cmd 当命令分隔符）。
- **post-build 必然报 `MSB3073`**（vcpkg 的 `applocal.ps1` 在本机起不来）——
  **不是编译失败**。判断依据：日志里出现 `<target>.vcxproj -> ...exe` 产出行。
- **运行期 DLL**：因上一条，`fmt.dll`/`spdlog.dll`/`glfw3.dll` 不会自动拷贝，
  缺了就 `0xC0000135`。从 `build\vcpkg_installed\x64-windows\bin`（Release）或
  `...\debug\bin`（Debug）复制到 exe 旁。
- **测试**：`build\tests\<Config>\moleculens_tests.exe`（当前 **133 用例 / 6943 断言**全过）。
- **基准**：`pwsh -File tools\run_bench.ps1 -Config Release` → `other\bench_out\bench_report.md`。
  引擎级基准程序是 `moleculens_bench`（headless GPU，`--rep/--material/--png/--verbose`）。
- **目录粒度规则**：任一目录 `.cpp/.h` ≤ 3 个（`cmake/check_tree.py`，现有 2 处豁免：
  `src/app/imgui/backends` 与 `tests/vulkan`）。
- **不要信 `git HEAD`**：工作区有大量 untracked 源码，基线是工作区本身。

## 已完成（对照，避免重复劳动）

- 渲染：批处理重构（**绘制调用 4084 → 2**）、单命令载荷 16 MiB 上限导致的**静默截断**
  （`appendTriMesh` 自动切块）、`drawSystemSurface` 越界写崩溃、PBC 分子补齐、
  表面网格缓存（材质改动 199→32 ms）、整体系表面默认开启。
- 表示 UI：「图形表示」面板（4 页签、27 绘图方法含未实现项置灰、
  23 套材质、Surface Quality 档位、Apply 机制、Selections 关键字来自解析器真实支持集）。
- 材质：23 套同名材质 + 透明度/光泽度等参数；**透明度此前完全无效**（管线选择）
  已修；`MaterialBlock` 字段带默认值（防未初始化触发随机管线）。
- 语言/主题：`app/i18n` 语言管理模块（中文源串为键）、白天/暗黑双模式、
  面板窗口**显示名与稳定 ID 分离**（`###`，否则切语言会丢 Dock 布局）。
- 轨迹：XTC 读取器（xdrfile 参考实现做金标准验证）、多实例体系轨迹、DCDC/XTC 分派。

## 本轮清理（2026-10-18，框架整洁化，已并入工作区）

- **移除未实现的 Label（标签）表示**：`RepresentationType::Label` / `drawLabelPlaceholder` /
  `LabelInst` 全部删除；枚举收缩为 13 种（Vdw=6 … EspSurface=12, Count=13），样式表/测试/
  序列化键同步更新。`Cmd::Label = 4` 作为 **v1 协议槽位保留**（旧 `.mlscene` 命令流兼容，
  decode 静默忽略；旧表示键 `"label"` 载入时回退 BallAndStick）。
- **Marching Cubes 查找表去重**：`surface.cpp` 私有的 4 张表（303 行）删除，统一使用
  `style/surface/mc_tables.h`（逐元素比对 256/4096/24/24 全部一致）。文件 732 → 434 行
  （−298 行，字节 −46.8%）；出图 SHA256 与清理前完全一致（行为零变化）。
- **死文件清理**：删除根目录历史残留 `main.cpp`（Hello World 桩）、`pkgconfig_stub.obj`、
  `test_export.*`（6 个导出测试产物）、`src/cli/cmd/render/esp_overlay_raster.h`（迁移后的
  兼容转发头，已无引用）、`tools/__pycache__/`。
- **阴影贴图尺寸收敛**：新增 `engine::kDefaultShadowMapSize`（render_parameter.h），实时/
  离屏两个后端共用，`LightParams::shadowMapSize` 默认值与之绑定（此前三处硬编码 2048）。
- **限帧精度修复**：主循环 `sleep_for` 受 Windows 计时器粒度影响（120FPS 目标被拖到 ~107FPS）。
  改为 `timeBeginPeriod(1)` + "睡到差 1.2ms 再自旋收尾"，精确落在帧预算上。
- **零警告构建**：修复 `application.cpp` 的 C4189（Release 下 consoleAttached 未使用）与
  `esp_surface.cpp` 的 C4267（size_t→uint32_t 显式转换）；Release 全量为 0 warning。
- **规约/文档**：`.gitignore` 增补 `.venv/`、`__pycache__/`、`build_agent/`；`scene_serializer`
  中 CRC32 的过时"占位"注释修正（CRC 实际已实现并校验）；删除 i18n 死词条 `{"标签","Label"}`。
- **保留项（非无用，未删）**：`shaders/surface.comp` + `src/engine/vulkan/compute/surface_compute.h`
  是 GPU compute 的预留实现（见上文 1.2 节，未接入构建）；`/other/` 为参考实现与基准产物
  （已 gitignore）；`软件注册/` 为软著/用户手册材料；`FIXES.md`/`verification/` 为上一轮修复证据。
- **验证**：Release 全量构建（app/cli/tests）0 warning；**156/156 测试通过**；ESP 出图
  SHA256 与清理前一致；i18n 覆盖率检查 OK。

