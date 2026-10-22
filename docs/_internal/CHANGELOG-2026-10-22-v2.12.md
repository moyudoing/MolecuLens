# v2.12 实施记录（2026-10-22）

> 范围：轨迹播放/交互回归、光追半透明、显示视图叠加、表示选择器、UI 清晰度、
> 新增「菱形 / 点云」表示与体系渲染模式入口。
> 验收基线：`build_dsh`（Debug）与 `build_rel`（Release）三件套 exit 0；
> `moleculens_tests` **182 例 / 193,749 断言全过**。
> 本文只记录"本轮做了什么、为什么、怎么验"；未解决项见 `ISSUES-已知问题登记.md`。

---

## 1. 轨迹播放：不能自动播放 / 流畅度低

### 根因

`FrameScheduler::advance()` 把 `dtFrame_`（轨迹的**物理**时间步长，md.xtc 是 10 ps）
当作实时步进间隔：

```cpp
accumulator_ += dtReal * speed_;
while (accumulator_ >= dtFrame_) { ... step(1); }   // ← 10 ps 被当成 10 秒
```

于是"播放"实际是每 10 秒一帧，用户看到的就是完全不动。同时 `TimelineController`
与 `FrameScheduler` 都没有"播放帧率"概念，UI 上也无从调整。

此外每帧帧切存在两处 O(n²)：

- `SceneStructure::applyTrajectoryFrame` 对 2042 个实例逐个调
  `writeFramePositions` → `globalAtomBase(i)` 内部 O(i) 累加；
- `Renderer::rebuildCommandList` 在实例循环里逐个调 `globalAtomBase(instIdx)`。

Debug 实测 `applyTrajectoryFrame` 单帧 **23 ms**，prepare 另 24 ms，播放只有 ~18–33 FPS。

### 修复

- `scheduler/frame_scheduler.{h,cpp}`：新增 `playbackFps`（默认 30，范围 1–240）；
  实时间隔 = `1 / playbackFps`，`speed` 只作倍率；物理 `dtFrame` 仅用于时间显示/导出。
  暂停/重新播放/手动 seek 时清空累积器，避免补帧跳变；单帧场景不再返回"已推进"。
- `TimelineController`：暴露 `setPlaybackFps/playbackFps`。
- `TrajectoryModule`：新增「播放帧率 12/20/24/30/60 FPS」与「实际推进 N FPS」提示。
  时间轴第一行同时显示当前播放帧率。
- `applyTrajectoryFrame`：按 `instances_` 顺序维护**递增段基址**，整段 O(n) 写回
  `coordsSoA_.positions`，脏位与坐标版本号只置一次（与原逐实例置脏语义等价）。
- `Renderer::rebuildCommandList`：每次重建先算一次逐实例原子基址前缀和；
  `Representer::generate` 新增可选 `globalAtomBaseOverride`，重放/正常路径共用该表。
- 播放期动态细节（`MolScene` + `OffscreenRenderer`）：坐标版本变化后 350 ms 内
  使用 **detail=2 / 12 段** 的低模球柱并**跳过阴影 pass**；暂停 350 ms 后自动回
  detail=3 / 24 段 + 阴影，并强制重绘一帧高清图。导出路径不参与（仍是 4/48）。
- 时间轴拖动条：命中区加高、按下即跳、悬停高亮；滚轮在时间轴上逐帧（与视口滚轮
  缩放命中区不重叠）。

### 验证

- Debug：`applyTrajectoryFrame` 23 ms → O(n)（配合渲染器前缀和，播放从 ~33 FPS 提升）；
  Release：`md.gro + md.xtc` 播放稳定 **117–119 FPS**（状态栏），30 FPS 播放速率准确。
- 单测：`FrameScheduler playback rate is independent of physical dt` 固化
  "1 秒推进 30 帧，与 dt=10 ps / 0.01 ps 无关"。

---

## 2. 加载轨迹后不能拖拽 / 滚轮缩放

### 根因

`Workbench::renderViewport` 在**时间轴子窗口渲染之后**才取 `ImGui::GetItemRectMin/Max()`
作为"分子画面矩形"，再铺相机输入 `InvisibleButton`。`GetItemRect()` 返回的是
"最后一个控件"——有轨迹时它是时间轴里的某个按钮，于是相机输入区整体错位，
视口里的拖拽旋转、滚轮缩放全部失效（无轨迹时恰好正常，所以只在导入后暴露）。

### 修复

- 在 `svc_.viewport->render(canvas)` 之后**立即**缓存 `imgMin/imgMax`，时间轴与标注
  在其后渲染，不再影响该矩形。
- 相机输入同时接受 `IsItemHovered()` 与 `IsItemActive()`：快速拖到视口外不会中断旋转。
- 悬浮工具条区域仍通过上一帧矩形排除；标注绘制使用同一矩形。

### 验证（GUI 实机）

导入 `data/md.gro + md.xtc` 后：左键拖拽分子旋转、滚轮缩放（比例尺从 20 Å → 10 Å）、
时间轴拖动/逐帧均正常。

---

## 3. 光追透明

### 根因

`buildRtScene` 一直把 `MaterialBlock` 的 ambient/specular/shininess/metallic 烘进
RT 图元，但**没有读取 opacity**，`shade()` 也不做混合 → 半透明表面在 RT 中完全实心。
用户看到的"光追引擎加载不了透明"即此。

### 修复（`engine/raytrace/ray_tracer.{h,cpp}`）

- `RtSphere / RtCylinder / RtTriangle` 新增 `opacity`，`buildRtScene` 从
  `MaterialBlock::opacity` 填充。
- 新增 `traceColor()`：命中面 α<1 时继续沿同一方向追踪后方，按
  `front·α + behind·(1−α)` 合成；深度上限 8 层，避免玻璃壳递归爆炸。
- 阴影光线跳过 α<0.9 的图元（与 GPU 侧"透明表面 shadowMask=0"一致）。
- α=1 的路径与旧 `shade()` 调用完全一致（不透明场景逐位不变）。

### 验证

- 新单测：透明球相对白背景的偏离显著小于不透明球；前透明红球 + 后不透明蓝球时
  中心像素带蓝（后方透出）。
- CLI：`polymer.gro` + 整体系 QuickSurf `Glass2` opacity 0.30 + 球棍，
  `--rt 1` 出图中球棍透过玻璃壳可见（`.monitor/rt_glass_new.png`）。

---

## 4. 显示视图的尺寸 / 坐标系方向（可关闭）

- `MolScene` 新增视图叠加层（draw list，不参与布局）：
  - **右上**：场景包围盒尺寸 `尺寸 X × Y × Z Å`（按坐标版本缓存）；
  - **右下**：比例尺，按相机 target 平面换算（透视/正交都成立），自动取 1/2/5×10ⁿ Å；
  - **左下**：世界 X/Y/Z 坐标轴方向（视图旋转投影，X 红 / Y 绿 / Z 蓝）。
- 开关：`显示` 面板「视图叠加」两个勾选；视口工具条「叠加 · 开/关」弹层（含全部开关）。
- `Å` 缺字修复：见 §6 的字体回退（此前显示为 `?`）。

---

## 5. 表示选择器与渲染模式

### 选择器

- 表格式表示选择器：点击「样式 / 配色 / 选择」单元格时**先把该行设为编辑目标**，
  再打开弹层（旧行为只开弹层，参数区仍编辑上一条表示——"点了没用"的主因）。
- 单元格文字追加 `▾`，明确可点击；工具提示写明"同时切换编辑目标"。
- 骨架样式（卡通/管状/改进卡通/丝带）在当前结构无 CA 骨架时**置灰**并给出原因；
  当前表示已是骨架样式但结构无骨架时，面板显示黄色警告 +「改回球棍」按钮。
  （旧实现：水盒/小分子上选中骨架样式直接白屏，无从理解。）
- 新增「菱形（宝石）」「点云」两项。

### 表示

- **Diamond**：每个 (链, 残基/分子) 一颗八面体宝石，TriMesh 面法线独立 → 棱面高光；
  颜色取平均原子色并按面做 ±7% 明度差。`md.gro` 2042 个水分子 = 2042 颗粉红宝石，
  溶质成较大青色宝石（`.monitor/diamond_md.png`）。
- **DotCloud**：样式内临时发 `RenderMode=Point`，渲染完恢复；点精灵按透视距离算大小
  （`sceneProps.y/z`），片元按 `gl_PointCoord` 裁成圆形并做球面明暗。
  `md.gro` 出图见 `.monitor/dot_md.png`。
- **Plate**：单原子/双原子残基不再跳过；单原子用世界轴小方片，双原子以连线为长轴。
- 网格重放：`SceneCommandList::beginReplay(allowMesh)`；Diamond/Plate/卡通等
  "顶点数不随坐标变化"的样式可参与轨迹坐标重放，Surface/QuickSurf 仍走全量重建。

### 体系渲染模式

- 实心/线框/透明/点 的入口从 v1 分子列表搬到 v2「分子」面板：每个体系一行
  `segmented` 控件，调用 `SceneController::setSystemRenderMode`。
- 点模式修复：`gl_PointSize` 原为固定 `radius×24`（远近同大）→ 改为按
  `viewportH / (2·tan(fov/2)·dist)` 透视换算；片元补圆形裁剪 + 剖切/雾处理。

---

## 6. UI 清晰度

- 字号 16/20/13 → **18/22/14 px**；`RasterizerMultiply=1.06` 略加粗细笔画。
- 默认字体已切换为 **Noto Sans SC 子集**（SIL OFL 1.1，见 `data/font/README.txt`），
  覆盖 `Å`/希腊/箭头；另保留系统微软雅黑 `MergeMode` 回退作为兜底。
  此前 OPPO_B 缺 `Å` 时界面显示 `?`，现比例尺/网格间距/盒子尺寸均正常。
- 浅色主题对比度：`textFaint` 2.5:1 → ~4:1；输入框底压深、边框加实、
  `FrameBorderSize=1`、`CellPadding(8,6)`、`DisabledAlpha 0.45→0.62`。
- 默认窗口自适应工作区（1920×1080 → **1440×900**），检查器/表格不再被挤得过密。

---

## 7. 验证清单

| 项 | 结果 |
|---|---|
| Debug 构建（canonical `build/` + clean `build_final`） | app / cli / tests exit 0；GUI 在锁屏前做过实机验证 |
| Release 构建（canonical `build/` + clean `build_rel`） | app / cli / tests 三件套 exit 0 |
| 单元测试 | **182 例 / 193,749 断言全过**（canonical Release/Debug、build_rel、build_final 各跑一次） |
| CLI 新表示 | `--rep diamond` / `--rep dotcloud` 出图非空、9×7 图分别 85 / 146 种颜色 |
| CLI 光追透明 | `--rt 1` + Glass2 0.30 表面：球棍透出壳外可见 |
| GUI 播放 | 30 FPS 帧号前进；暂停后 117–119 FPS |
| GUI 拖拽/缩放 | 导入轨迹后左键旋转、滚轮缩放正常 |
| GUI 叠加 | 尺寸/比例尺/坐标轴均可见；两个开关分别关闭生效 |
| GUI 选择器 | 弹层选择生效（键盘 / 精确鼠标点选）；骨架样式置灰 + 警告已目视。单元格点击"先切换编辑目标"为逻辑修复，锁屏前未单独复验 |
| 渲染模式 | 实心/线框/透明/点切换即时生效（点模式大小随距离） |

> 注 1：测试使用工作区临时目录（`TEMP=F:\MolecuLens\_testtmp`）。`C:\Users\...\Temp`
> 在当前环境对子进程写入偶发被拒（与 10-18 记录的 `test_export.*` 环境问题同类），
> 不是代码缺陷。
>
> 注 2（构建坑）：本轮改过 `SceneStructure` 布局与 `Representer::generate` 签名后，
> 一次增量构建出现"测试对象未重编、链接到旧 ABI"→ 运行期 heap corruption（0xC0000374）
> 的假象；clean rebuild 后全部通过。**凡改公开类布局/函数签名，验收前必须 clean build**，
> 不要把崩溃直接归因于业务代码。另外，崩溃的 `moleculens_tests.exe` 可能以
> "0 句柄 zombie" 形态残留并锁住 exe（`taskkill` 也报拒绝访问）——换新构建树或重启后再验收。

---

## 8. 产物路径

- 可直接运行（canonical Release）：
  `build/src/app/Release/moleculens_app.exe`、`build/src/cli/Release/moleculens-cli.exe`、
  `build/tests/Release/moleculens_tests.exe`（本机 1920×1080，窗口默认 1440×900）。
- 本会话独立构建树：`build_rel/`（Release，clean）与 `build_final/`（Debug tests，clean）；
  `build_dsh/` 为开发中增量树。
- 启动示例：`moleculens_app.exe data\md.gro data\md.xtc`（工作目录需含 `font/` 与 DLL）。

## 10. 轨迹动画导出（11:15 追加：CLI + GUI；12:00 修复逐帧闪烁）

- 新增 `engine/export/anim/anim_encoder.{h,cpp}`：定位 ffmpeg（`PATH` 或 `MOLECULENS_FFMPEG`），
  把 `frame_%04d.png` 封装为 MP4（H.264）/ WebM（VP9）/ GIF（单趟 palettegen+paletteuse）/ APNG；
  失败时回传 ffmpeg 的最后几行输出，不再只报"未接入"。
- GUI「文件 → 导出图像 → 动画导出」接上编码器：非 PNG 序列先渲染到 `<输出路径>.frames`，
  封装成功后清理该目录；输出无扩展名时按格式自动补 `.mp4/.webm/.gif/.apng`。
- 帧命名改为**顺序** `frame_0000.png`（旧实现按轨迹帧号，起始帧≠0 或步长>1 时序列有空洞）。
- CLI 新增 `--anim <out>`、`--anim-fps`、`--anim-range a:b`、`--anim-step N`、`--anim-interp K`。

### 逐帧闪烁的三处根因（已修）

1. **CLI 只写了 instance 0**：`--traj` 路径把整帧坐标写进第 0 个实例（一个水分子），
   其余 2041 个实例保持首帧坐标 → 视频里只有一个小点在闪，整片溶剂不动。
   改为按 `instances_` 切片逐实例写回（`applySystemFrameToScene`）。
2. **缺 PBC 补齐**：轨迹坐标是 wrapped 的，跨盒分子被画成 ~40 Å 长棍，播放时整根棍闪动。
   改为逐分子 `unwrapAroundFirst` 后再整体折回盒子（分子完整、画面不散开）；
   动画路径另维护一份**时间轴连续缓冲**（`unwrapAcrossTime`，以上一帧连续位置推进锚点），
   插值只在连续缓冲里做，避免跨盒分子在中间帧被拉成长棍。
3. **采样太粗 + 无插值**：`md.xtc` 相邻帧的平均原子位移实测 ~5 Å（远大于 10 ps 水的扩散，
   实际存帧间隔可能比 dt 元数据粗），30 fps 直接播放就是"瞬移"。
   新增 `--anim-interp K`：在连续缓冲上做帧间线性插值（演示用 K=16，每帧位移压到 ~0.3 Å）。
- 另外：动画逐帧换整体系坐标时**强制整条命令流重建**（`setDirty(DIRTY_INSTANCE)`），
  绕开坐标重放路径在整体系换坐标时的错位风险。

**验证**：Debug 构建 exit 0；**184 例 / 193,765 断言全过**；CLI 实测 `md.gro + md.xtc`：
- MP4 640×480@20fps、21 帧、H.264/yuv420p；GIF、APNG 均有效（ffmpeg 4.0.2）；
- 逐帧位移自检：interp=4 时每个渲染帧平均位移 1.23 Å（源码帧 4.93 Å）；
- 演示 `trajectory_smooth.mp4`：800×600→640×480、31.5 s、interp=16，邻帧位移 ~0.3 Å。



- Debug 下播放受"未优化代码 + 逐帧 GPU 同步"限制（修复前实测 33 FPS；O(n²) 已消除，
  尚未重新测 Debug 数值）；Release 稳定 117–119 FPS。
- 自适应距离 LOD（按投影像素直径选档）仍开放（`ROADMAP §2.1`；本轮只做了播放低模档）。
- RT 半透明是单次前向合成，无深度排序/折射（`ISSUES I-28`）。
- 分子名标注仍对每实例调 O(n) 的 `globalAtomBase`（`ISSUES I-27`，默认关闭标注）。
