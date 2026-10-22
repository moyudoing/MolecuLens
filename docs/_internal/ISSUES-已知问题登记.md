# MolecuLens · 未解决问题登记

> 快照：2026-10-22 13:55（Asia/Shanghai），代码基线 v2.12。
> 范围：**只登记尚未解决**的问题。已修复/已结案的条目在 2026-10-22 清理中移出，见
> [`archive/ISSUES-已知问题登记-20261022-全量.md`](archive/ISSUES-已知问题登记-20261022-全量.md)（含每条修复过程与实测数字）。
> 严重度：**S1** 用户能直接看到的功能/视觉错误（当前无）；**S2** 影响体验或工具的缺陷；**S3** 噪声 / 待验证 / 流程问题。
> 维护：修好一项就把该条移入 `archive/README.md` 的"不要重做"清单并删掉本表行，不留已完成条目。

---

## 一、未解决清单

| ID | 问题 | 严重度 | 状态 | 关键位置 |
|---|---|---|---|---|
| I-10 | 「场景有原子但命令流无几何」→ 该帧视口不重绘 | S2 | 偶发、未修 | `src/app/view/viewport/mol_scene.cpp:478` |
| I-13 | GUI 目视复验尾账：半透明表面暗斑、平板表示 | S2 | 待解锁桌面补测 | `_test/gui_verify/` |
| I-14 | 正式文档 01–05 与 DOCX 落后于代码（v2.2–v2.12） | S2 | 未做 | `docs/01`…`docs/05` |
| I-21 | 导入新结构/体系后不自动取景，新体系可能贴边 | S2 | 未做 | `src/app/controller/io/io_controller.cpp` |
| I-29 | 动画坐标管线 GUI/CLI 两套（GUI 无插值/时间轴展开） | S2 | 未统一 | `ExportImgTool::exportAnimation` / `render_cmd.cpp` |
| I-30 | 命令流坐标重放：整体系换坐标时错位（CLI 动画强制整条重建） | S2 | 规避中 | `render_cmd.cpp` 动画分支 / `Renderer::prepare` |
| I-09 | 偶发 100 ms 长帧 / 190 ms 工作块尖峰 | S3 | 未查 | 性能日志 `[perf]` |
| I-11 | SDF 属性块被当 counts 行 → 整条记录跳过、属性丢失 | S3 | 未修 | `src/io/sdf/sdf_parser.cpp:171` |
| I-17 | 逐实例 QuickSurf 缓存：实现后回退，待决策 | S3 | 待决策 | `src/engine/style/quicksurf/quicksurf.cpp` |
| I-22 | 轨迹按体系聚合，同体系重复导入会覆盖上一条 | S3 | 未做 | `src/app/workbench/modules/modules.cpp` |
| I-23 | 分子面板每帧重建分组（2042 实例规模） | S3 | 可接受、未优化 | `renderMoleculeSection()` |
| I-24 | 分子列表聚合后无法单独选中某个实例 | S3 | 未做 | 同上 |
| I-26 | 选择表达式无语法预校验，非法表达式到渲染期才暴露为空 | S3 | 未做 | `RepresentationController::setSelection` |
| I-27 | 分子名称标注每帧对每个实例调 `globalAtomBase`（2042 实例时 O(n²)） | S3 | 未优化（默认关闭标注） | `workbench.cpp` 的 `showMolLabels_` 分支 |
| I-28 | RT 半透明只做"前表面 × α + 后方"混合，没有深度排序/折射/多次散射 | S3 | 近似、未做 | `src/engine/raytrace/ray_tracer.cpp::traceColor` |
| I-31 | 动画导出无自动化测试与回归基线 | S3 | 未做 | `tests/`（无动画用例） |
| I-32 | `render_cmd.cpp` 973 行单函数，动画逻辑继续堆叠 | S3 | 待拆 | 见 REFACTOR P1-1 |
| I-33 | `md.xtc` 相邻帧位移 ~5 Å，与 dt=10 ps 不符（存帧间隔待核实） | S3 | 待核实 | `data/README.md`、`data/md.xtc` |

---

## 二、S2（体验 / 工具）

### I-10 「场景有原子但命令流无几何 → 视口不会重绘」
- **现象**：视口门控（`dirty_ / cameraMoved / sceneDirty` 全 false 但场景有原子）命中时，只有一条 `spdlog::warn`，该帧不重绘，画面可能停在旧帧。
- **位置**：`src/app/view/viewport/mol_scene.cpp:478`；10-21 13:16 出现一次，历史多次。
- **下一步**：加一条调试开关，命中时强制置脏并打点（serial、原子数、表示数），确认是否与"切表示后命令流短暂为空"同源。

### I-13 GUI 目视复验尾账
已确认：`md.gro + md.xtc` 轨迹播放（v2.12 实机：30 FPS 播放、拖拽旋转/滚轮缩放、时间轴拖动均正常）、
标注随 PNG 导出（v2.9）、新增「菱形 / 点云」表示与视图叠加（v2.12）。
**仍缺**：
1. 半透明表面「幽灵剪影」暗斑是否消失（EPI.pdb + 半透明表面）；
2. 平板（Plate）表示目视：`md.gro` 应为 2042 块方片、`EPI.gro` 单块。
- **约定**：截图落 `_test/gui_verify/`，结论回填 `docs/04-测试报告.md` §5.9，不要另开散文档。
- **前置**：需要解锁桌面（锁屏下 GUI 无法创建窗口）。

### I-14 正式文档与 DOCX 落后
- **现状**：`docs/01`–`docs/05` 的 `.md` 为 10-21 12:16–12:18，`.docx` 为 12:18；之后落地的标注模块、Plate/Diamond/DotCloud 三种表示、渲染风格预设、v2.7–v2.11 的 UI 改动都没写进去。
- **另需**：`docs/figs_v2/` 中 10-21 15:50（sRGB 修正）之前的插图按新色域复核/重出；`docs/04` 的用例数更新为最近记录 **184 例 / 193,765 断言**。
- **做法**：以代码为准逐节核对 → 改 `.md` → `python tools/md_to_docx.py` 重出 DOCX → 同步 README 的里程碑表。

### I-21 导入后不自动取景
- **现象**：再打开一个结构文件，新体系沿 +X 并排（v2.10 已修重叠），但相机不动，新体系可能落在视野外；只能按 `F` 或点视口左上「取景」。
- **现状**：`frameCamera()` 只在菜单、快捷键 `F`、悬浮按钮里调用，导入路径没有调用。
- **建议**：`SceneOpened` 时若检测到"体系数增加"，自动 `frameCamera()`（或只在新体系包围盒不在当前视锥内时取景）。

---

## 三、S3（待查 / 待决策 / 小修）

### I-09 偶发长帧
- 10-21 13:17–13:21 记录到 `max 100.00 ms`、`工作量 max 189.92 ms`，最低瞬时 10 FPS，随后自恢复；此后未复现。
- **建议**：下次复现时抓一帧 CPU 侧剖析（命令列表重建 / GPU 上传 / 选择重排），先确认与 I-10 同源否。

### I-11 SDF 属性块被当 counts 行
- `sdf_parser.cpp:171-192` 只认带 `V2000` 的 counts 行；读 PubChem 导出的 SDF 时属性标签 `> <PUBCHEM_COMPOUND_CID>` 被当成 counts 行 → 打 `unsupported SDF/MOL version` 警告并跳过整条记录。
- 主结构不受影响，但属性丢失。**建议**：counts 行解析失败时向后探一行重试，或先把 `> <` 开头的块按属性处理。

### I-17 逐实例 QuickSurf 缓存（待决策）
- 键：结构索引 + 半径缩放 + 网格间距 + 等值面 + 掩码模式；缓存以首个原子为原点的局部网格，其余实例平移复用。
- 收益：命中率 7/2042，Debug 11867 → 5650 ms（2.1×）。代价：局部栅格的 MC 相位与世界栅格不同，像素差 21.9%（几何等价）。
- 已回退，当前输出与基线逐位一致。**可选项**：① 接受相位变化并重出插图；② 键里加入 `floor(pos/sp)` 的小余数做相位分桶（命中率下降、输出与旧版一致）。
- 背景：GUI 里对溶剂盒选 QuickSurf 会走"每分子一个等值面"（846 ms / 316 MB 命令流），是用户能感知的卡顿。

### I-22 轨迹「每个体系一条」
- 轨迹面板按体系聚合；同一体系再次导入会覆盖上一条，没有多文件并存/切换。
- **建议**：改为按 `(systemId, source)` 列表，或导入时提示"将替换该体系现有轨迹"。

### I-23 分子面板每帧重建分组
- `renderMoleculeSection()` 每帧重扫实例与体系（2042 实例规模下可接受，v2.10 已实测不卡）。
- **建议**：按 `sceneSerial` / 体系版本号缓存分组结果，顺带为 I-24 的展开态提供稳定键。

### I-24 聚合后无法选中单个实例
- 分子列表聚合为「体系 → 结构 ×N」后，没有展开实例的二级列表。
- **建议**：行内加展开箭头，子项写 `实例名 (原子数)`，选中后仍写入 `selectedMolecule()`。

### I-27 分子名称标注的 `globalAtomBase` 热点
- 开启「分子名」标注后，`renderViewport` 对每个实例调一次 `globalAtomBase(i)`；该函数是 O(i) 累加，
  2042 实例时整体 O(n²)（与 v2.12 已修的轨迹帧切同类）。默认关闭标注，暂不影响日常使用。
- **建议**：渲染器已在前缀和里算好逐实例基址；把该表暴露给视图层复用（或给 `SceneStructure` 加一份
  带失效标记的缓存），顺带覆盖 picking 的同类调用。

### I-28 RT 半透明的近似边界
- v2.12 已让 RT 按 `opacity` 与后方景色做 alpha 混合（玻璃壳内球棍可见），但仍是**单次前向合成**：
  没有深度排序、折射、多次散射；多层半透明表面叠加时与 GPU 的粗粒度两遍绘制不完全一致。
- **建议**：需要更接近 VMD Tachyon 时再做深度排序 + Fresnel/折射近似；当前作为"观感一致"档已够用。

### I-26 选择表达式无预校验
- `RepresentationController::setSelection` 只赋值 + `touch()`；非法表达式在渲染期才表现为空选择（视口空白）。
- **建议**：UI 上用 `Selection` 解析器预校验，非法时红框提示并保留上一个有效表达式。

### I-29 动画坐标管线两套
- **现状**：CLI 动画用 `applySystemFrameToScene`（本次新增：逐帧 PBC 补齐 + 分子回盒 + 时间轴连续缓冲 + 插值）；
  GUI 动画用 `SceneStructure::applyTrajectoryFrame`（只有帧内 PBC 补齐，无时间轴展开/插值）。
- **影响**：同一条轨迹 GUI 导出与 CLI 导出观感不同；GUI 对大帧距轨迹仍会跳。
- **方案**：统一到 `SceneStructure::applyFrame(...)`（见 `REFACTOR-2026-10-22.md` P0-1 / A1）。

### I-30 命令流坐标重放不可靠
- **现象**：动画逐帧换**整体系**坐标时，走坐标重放路径的画面像随机重排；当前用
  `setDirty(DIRTY_INSTANCE)` 强制整条重建规避，CLI 动画只有 ~6–7 fps（800×600）。
- **影响**：导出慢 5–10×；GUI 逐帧播放同样走 replay，存在同源风险。
- **方案**：补"同帧 replay vs 整条重建逐像素一致"单测，修复后去掉强制重建。

### I-31 动画导出无自动化测试
- **缺**：帧文件名顺序、插值后位移减半、PBC 连续性、ffmpeg 缺失时的回退/报错都没有用例。
- **方案**：20 帧小夹具 + `MOLECULENS_FFMPEG` 指向 stub 二进制（见 REFACTOR A4）。

### I-32 `render_cmd.cpp` 单函数过大
- 973 行、解析/场景/表示/渲染/动画全在一个 lambda 里；本次动画又加了 ~150 行。
- **方案**：拆 `render_options` / `render_scene` / `render_frame` / `render_anim`（REFACTOR P1-1）。

### I-33 `md.xtc` 采样语义待核实
- 动画自检：相邻帧平均原子位移 ~4.9 Å（interp=4 时每渲染帧 1.23 Å）。若 `dt=10 ps`，
  这与常温水的扩散速度不符，**实际存帧间隔可能比元数据更粗**。
- **方案**：读 XTC 每帧 time 字段核对（header 有 time），更正 `data/README.md`；
  在核实前，`--anim-interp` 只是显示层补偿，不是数据修正。

---

## 四、建议顺序

1. **I-29 + I-30**（动画管线统一 / 重放修复：直接决定导出正确性与速度，见 REFACTOR 阶段 A）
2. **I-21**（导入即取景，10 行级，直接消除"看不见新文件"）→ **I-26**（空选择提示，避免同类困惑）
3. **I-13 + I-14**（解锁桌面后一次做完：目视补测 → 改 01–05 → 重出 DOCX → 同步 README）
4. **I-10**（加重绘打点，可能一并解释 I-09）→ **I-33**（核对轨迹采样，解释动画节奏）
5. **I-17**（先定"要不要相位差"，再决定是否拿回 2×）
6. **I-31 / I-32**（动画单测与 CLI 拆分，随 REFACTOR 阶段 A/B 一起做）
7. **I-11 / I-22 / I-24**（小修，各自带单测）
8. **I-23 / I-27**（有性能诉求时再做；I-27 与轨迹帧切同源，修法已明确）

---

## 五、已关闭（本轮已归档，不再跟踪）

I-01 线框一链一色、I-02 GRO 伪链、I-03 Residue 按名去重、I-04 配色名缺 AtomName、I-05 `colorNames[5]` 越界、
I-06 调色板 sRGB/线性错配、I-07 线框默认配色、I-08 64 FPS 天花板（实为 120 FPS 限帧器）、
I-12 CLI 无参挂起（`main.cpp` 已打印 usage 并返回 3，`--help` 已走 CLI11 帮助路径）、
I-15/I-16 构建链两坑（dumpbin 进 PATH、`TEST_CASE` 名英文化）、I-18 RT 切分辨率刷 `vkQueueSubmit failed`、
I-19 新建表示引用悬垂崩溃、I-20 表格内行级 PushID 冲突断言、
I-25 CPU 光追忽略表面不透明度（v2.12：`traceColor` 按 opacity 与后方 alpha 合成，半透明表面已可光追）。

修复过程与实测数字：`archive/ISSUES-已知问题登记-20261022-全量.md`。

---

## 六、仍然有效的环境坑（不是缺陷，但排查前先看）

1. **构建噪声**：vcpkg post-build 必报 `MSB3073`（`applocal.ps1` 找不到 dumpbin 的残留输出）。判定成功看链接行 `-> ...exe`。
2. **运行期 DLL**：`fmt.dll` / `spdlog.dll` / `glfw3.dll` 不会自动拷贝，缺了报 `0xC0000135`；从 `build\vcpkg_installed\x64-windows\bin`（Release）或 `...\debug\bin` 复制到 exe 旁。
3. **着色器是 configure 期编译的**：`cmake/shaders.cmake` 在 configure 阶段用 `glslangValidator` 生成 `build/gen/shaders/*.h`。改 `shaders/*.frag|glsl` 后只跑增量构建**不会**重新生成内嵌 SPIR-V，必须重新 `cmake -S . -B build`；判断方法看 `build/gen/shaders/mesh_frag.h` 的修改时间。
4. **日志路径随启动目录变化**：日志写在**当前工作目录**的 `logs\moleculens.log`，同一 exe 可能有多份；排查"日志停更 = 卡死"前先确认哪份在涨（看 mtime）。
5. **桌面锁屏**会让 GLFW 窗口创建失败（进程存活、日志零输出），所有 GUI 验收需解锁。
6. **GUI 授权按 agent 会话隔离**：子会话调 `request_access` 会一直卡审批，GUI 操作在主会话做。
7. **改类布局/函数签名后必须 clean build**：本轮增量构建曾漏编测试对象 → 新旧 ABI 混链，
   表现为运行期 `0xC0000374` heap corruption（与业务代码无关）。崩溃的测试进程还会以
   "0 句柄 zombie" 残留并锁住 exe，`taskkill` 报拒绝访问；换新构建树或重启后再验收。
8. **`TEST_CASE` 名只写英文**：CMake 在 GBK 代码页（936）下解析 Catch2 JSON 会因中文用例名失败；中文放注释或断言消息。
9. **powershell 写文件别拼字符串**：`` `0 `` 是 NUL、弯引号会被当定界符；多行文本用文件工具写。
