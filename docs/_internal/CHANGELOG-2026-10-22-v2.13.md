# MolecuLens v2.13 · 2026-10-22

渲染视图交互重做 + 多表示/快速表面轨迹播放性能修复。

## 轨迹 + 快速表面播放（本次核心修复）

**症状**：同时开启球棍与整体系 QuickSurf 后播放轨迹，界面卡住、帧几乎不前进（Debug 下 14 FPS，
每帧都在等 async 表面任务）。根因不是“每帧必须重算表面”，而是：

1. 冻结开关没有覆盖整体系表面的命令流：坐标重放时仍进入 `systemSurface` 分支，
   重新发射 RenderMode/MaterialBlock/TriMesh，触发断言或整帧全量重建。
2. 表面生成完成不会自动让命令流失效，表面要等下一次“无关操作”才出现。
3. 即使表面几何没变，`drawGeometry` 仍按命令流 serial 每帧重传整张表面网格。

**修复**（`src/engine/render/renderer.cpp`、`src/engine/cmd/cmd.*`、`src/engine/vulkan/draw/draw_geometry.*`）：

- `sceneMeshReplaySafe()` 按表示类型分类：冻结的整体系表面允许留在旧 TriMesh 中只重放坐标；
  逐实例表面仍必须全量重建。重放计划里用 `skipReplayMesh()` 消费掉对应的网格记录，避免游标错位。
- `sceneNeedsCoordUpdate()`：只有动态几何（球棍/线框/点云等）需要坐标重放；仅“冻结整体系表面”
  时直接复用上一帧命令流，不再每帧走重放。
- `MolScene::render` 的 gateOpen 纳入 `surfaceGenerationPending()`，后台表面完成后会持续出帧，
  表面不必再靠用户点一下别的表示才刷新。
- 新增 `SceneCommandList::meshSerial()`：全量重建才递增。后端缓存 `meshUploadRevision`，
  坐标重放时跳过表面顶点/索引上传和材质烘焙；`listRevision` 仍用于实例缓冲更新。
- 冻结期间不等待 async 表面；暂停后 350 ms 动态窗口结束自动解冻并重建当前帧表面。

### 逐帧预计算（第二轮）

**需求**：快速表面必须跟随轨迹逐帧变化，而不是“冻结一张旧表面”。

- `SceneStructure::buildWorldPositionsForFrame(frame, out)`：只读地取出任意未来帧的世界坐标，
  不修改当前坐标、不置脏。
- 时间轴帧号经 `Workbench -> MolScene::setTrajectoryFrame -> Renderer::setTrajectoryFrame`
  进入 `RepContext::frameIndex`，表面指纹因此包含帧号。
- `quicksurf.cpp` 的单槽缓存换成 24 帧 LRU（`CachedSurface` + `SurfaceMeshCache`）；
  当前帧未命中时先显示上一帧表面，同时提交当前帧任务。
- 当前帧提交后，`prefetchSystemQuickSurf` 用**低优先级队列**预取后 6 帧
  （`submitPrefetchSurfaceJob`），不阻塞当前帧、不延长 `prepareBlocking` 的等待时间。
- `surfaceJobsSubmitted()` 只统计前台任务，异步完成后的重建判断不会把预取任务算进去；
  `shutdownSurfaceJobs` 同时清理前台与预取队列。

**实测**（同场景 Release）：播放时整体 98–119 FPS，轨迹稳定 30 FPS；表面随帧更新
（日志确认多帧 `buildSystemQuickSurfMesh` 完成，包围盒尺寸变化）；暂停时停在正确帧，
不再需要“点一下别的表示”来刷新。

## 渲染视图 UI

- 表示选择从表格改为**列表 + 下方表单**：列表行显示可见性、名称、类型、配色；选中行
  自绘中等饱和玫瑰色底 + 4 px 深色竖条 + 描边（浅色主题下不再依赖几乎看不见的浅粉 Header）。
- “显示视图”更名为“渲染视图”。
- 新建表示先选类型，名称/作用分子/绘图样式/配色/材质等在下方编辑。
- 材质预设名称中文化（不透明/透明/拉丝金属/漫反射/幽灵/玻璃 1–3）。
- 新增 `菱形（非氢原子）` 与 `多面体（非氢原子）`；菱形改为每个非氢原子一颗，不再按分子。
- ESP 色标/极值标注在有轨迹时间轴时上移 74 px，避免被时间轴遮挡。

## 窗口拖动闪烁

**症状**：抓住标题栏拖动窗口时，视口内容闪烁/抖动。

**根因**：`ws.dragWindow` 每帧用 `glfwSetWindowPos` 直接改窗口位置。窗口移动、Vulkan
呈现与垂直同步同时在跑，DWM 合成时看到的是不同帧的窗口表面；快速拖动时尤其明显。

**修复**（`src/app/view/control/view_control.cpp`）：

- 拖动标题栏不再逐帧 `glfwSetWindowPos`，第一次越过 ImGui 拖动阈值后调用
  `beginNativeWindowDrag()`：`ReleaseCapture()` + `SendMessageW(hwnd, WM_NCLBUTTONDOWN,
  HTCAPTION, 0)`，把移动交给 Windows 原生非客户区移动循环。
- 原生循环期间窗口共用同一张最后呈现的表面，由 DWM 连续移动，不再出现每帧重定位的闪烁；
  松手后 `SendMessageW` 返回，主循环继续渲染。
- 单击/双击不会进入移动循环（先判断 `IsMouseDragging`），窗口按钮与菜单行为不变。

## 导出与手势（补充）

- **GUI 图片保存“没有权限”**：导出对话框的默认路径原为进程当前目录（`build/src/app/Debug`），
  普通用户对该目录不可写；另存为/默认输出改为 `图片/MolecuLens`（回退 `文档`、临时目录），
  并校验目录可创建。`Ctrl+E` 与工具栏“导出 PNG”均走同一路径。
- **轨迹视频导出**：Aurora 的导出弹窗改为“图片 / 动画”两页；动画页可选题材（轨迹 / 相机旋转 /
  轨迹+相机）、格式（PNG 序列 / APNG / GIF / MP4 / WebM）、fps、起止帧、步长和输出路径。
  视频与动图先写临时帧目录，再交给 `engine::encodeAnimation`（ffmpeg）封装；默认
  `%USERPROFILE%/Pictures/MolecuLens/trajectory.mp4`，相对路径会解析到同一目录。
  GUI 已点测：`md.gro + md.xtc` 全 1001 帧导出为 `trajectory.mp4`（52,223,474 字节，
  808×608、30 fps、时长 33.37 s），临时帧目录导出后自动清理。
  导出逐帧会改写场景坐标；结束（成功或失败）后自动恢复导出前的时间轴帧，界面不会停在末帧。
- **放大后拖拽“翻形”**：近裁剪面改为跟随相机距离收缩（`nearClip = min(r·1e-3, distance·1e-3)`），
  最近距离限制为包围球半径的 0.15 倍，俯仰角钳制在 ±1.55 rad，避免相机钻进分子或越过极点时
  近裁面把球/键切成断面、lookAt 上方向翻转。
- **鼠标手势**：左键拖拽方向反转（拖右→分子右侧转过来）；右键拖拽改为绕屏幕法线滚转
  （更顺手的 tumble/spin 手感），Shift+右键保留屏幕空间平移；中键/滚轮 dolly 不变。

## 逐帧 PBC 按分子补齐（“分子有时畸形”）

- 旧实现每个实例只用**第一个原子**做锚做最小镜像。当另一个分子恰好位于半个盒长附近
  （如锚在 x=0.5，分子原子在 x=19.0 / 20.2）时，两个原子会选到相反镜像，键被拉成
  贯穿盒子的长棍，播放到某些帧尤其明显。
- `MoleculeStructure` 新增 `unwrapRoot`：`registerStructure` 时按键连接做并查集，记录
  每个原子所属分子分量的最小下标；`unwrapByComponents` 以每个分量的最小原子为锚，
  逐分量做最小镜像。`applyTrajectoryFrame`、`buildWorldPositionsForFrame`、静态首帧
  填充三条路径统一使用。
- 新增回归测试：两个双原子分子分别位于 0.5/1.5 与 19.0/20.2，断言两者键长仍为
  1.0 Å / 1.2 Å，而不是 38 Å 长棍。

## 验证

- Debug：189 test cases / 193,789 assertions 全通过。
- Release：`moleculens_app.exe`、`moleculens-cli.exe` 已重建。
- 新增回归测试：`frozen system surface survives coordinate replay`（冻结重放保留 TriMesh、
  `meshSerial` 不变；解冻后全量重建并递增）、`prefetch jobs keep the normal queue idle`
  （低优先级预取不进入 `prepareBlocking` 的等待集合）。
- GUI 实测：逐帧 QuickSurf 播放 98–119 FPS；菱形/多面体按非氢原子渲染；ESP 色标位于
  时间轴上方；材质名称中文；选中行对比度已加强；原生标题栏拖动正常（Debug/Release 各拖动
  两次，窗口位移与鼠标一致、无跳变）。
- GUI 实测（本轮）：`md.gro + md.xtc` 播放 118 FPS；左键拖拽方向自然、近距离放大后球体
  无可见断面；动画导出页 1001 帧 MP4 成功，导出后回到原时间轴帧；逐分子 PBC 补齐后
  跨盒边界帧未见长棍。

## 已知后续

- 动画导出仍在主线程同步执行（避免与渲染线程竞争场景坐标），大轨迹整段导出时界面会短暂
  无响应（本轮 1001 帧约 7 s）；下一轮可加进度条/取消与后台安全快照。
- “畸形”如仍出现在特定文件/操作，需要用户给出复现帧与表示方式；当前已覆盖 PBC 跨盒
  长棍、近裁剪断面与 lookAt 极点翻转三类已知原因。

## 选中态统一为蓝底白字（v2.14）

**反馈**：左侧工作区选中项（如“渲染视图”）底色只有 16% 主色，图标/文字又是主色，
浅色主题下几乎看不出来；用户建议选中项用蓝色实底、图标变白，其他组件同样处理。

- `Palette` 新增 `selection` / `onSelection`：固定高饱和蓝（浅色 37,99,235；深色同色）
  与纯白前景，不再从可自定义/偏粉的 `accentSoft` 推导。
- `navButton`：选中项画实心蓝底，图标与文字用白色，左侧半透明白条；hover 白描边。
- 表示列表选中行：蓝底 + 白字 + 4px 白竖条 + 1px 白描边，可见性复选框仍在蓝底上。
- `chip` / `segmented` 的激活态也改用蓝底白字，避免自定义粉色主色时白字对比不足。
- 验证：Debug/Release 189 test cases / 193,789 assertions 全通过；Release 截图确认
  “渲染视图”蓝底白图标、ballstick 行蓝底白字，其余未选中项保持原样。

## 轨迹播放流畅度：跟随刷新率（v2.15）

**反馈**：轨迹播放流畅度很差，要求向成熟查看器的播放手感看齐。

- 成熟实现的动画节奏是**每个显示帧检查一次**、每次重绘推进一步，因此默认不是固定
  30 FPS，而是与显示刷新同步。
- 旧实现默认 `playbackFps_=30`：渲染 118 FPS 时轨迹仍只按 30 次/秒跳帧，肉眼就是
  “一顿一顿”，与渲染帧率无关。
- `FrameScheduler`：`setPlaybackFps(0)` 表示刷新率同步；`advance()` 在同步模式下把
  本帧真实 dt 作为间隔，每个渲染帧推进一帧；速度倍率仍生效（0.5× = 两帧一帧，
  2× = 一帧两帧）。默认改为 0（同步）。
- UI：轨迹模块“播放帧率”新增首项“刷新率同步”，实际推进显示“刷新率同步 × 1.00”；
  时间轴状态文字显示“刷新同步”。需要给 QuickSurf 预计算留余量时可切回 12/20/24/30/60 FPS。
- 新增测试 `FrameScheduler refresh-synced playback`：默认同步；120 Hz 下每帧一帧；
  0.5× 两帧一帧；2× 一帧两帧。
- 验证：Debug/Release 190 test cases / 193,799 assertions 全通过。Release 用
  `md.gro + md.xtc` 实测：播放 4 s 从第 1 帧走到约第 504 帧，整体 119 FPS；
  暂停在第 742 帧仍为 117 FPS，无明显跳帧。
- Debug 构建同步播放约 33 FPS（每帧全量重建坐标命令的调试开销），Release 为 119 FPS；
  性能结论以 Release 为准。

## 参考图风格预设与蛋白选择修正（v2.16）

**需求**：实现用户提供的 10 张参考图里的绘图风格，并整理可复用的参考素材。

- 新增 `样式 → 参考图风格` 菜单（`src/app/workbench/workbench.cpp` `applyPreset`）：
  `kOrbitalDark` 深色轨道、`kPublicationCartoon` 出版卡通、`kMofPolyhedra` MOF 多面体、
  `kCleanFigure` 干净出图，以及 `kClearStyle` 恢复默认。
- 深色轨道：近黑蓝渐变底、细球棍、低饱和元素色、柔和补光；适合 HOMO/LUMO 等
  轨道等值面叠加（轨道本身的渲染仍由“轨道”表示提供）。
- 出版卡通：白底、关阴影、`Cartoon` 平面飘带（不是 `NewCartoon` 圆管）+
  配体球棍；蛋白表面作为可选表示加入列表但**默认隐藏**，避免实时管线不透明
  表面遮住卡通。需要使用表面时在列表中勾选。
- MOF 多面体：非氢原子多面体 + 细球棍 + 晶胞框，白底、关阴影。
- 干净出图：保留当前表示，只统一白底、柔光、关阴影，适合快速截图。
- **蛋白选择修正**：选择语言的 `protein`/`backbone`/`sidechain`/`waters` 仍是
  未实现占位宏（`src/entity/selection/selection.cpp:36`）。出版卡通改用按残基名
  生成的显式表达式（20 种标准氨基酸，`proteinResidueSelection()`），并在
  `sceneHasProtein()` 为假时退化为出版球棍，避免空卡通。
- 验证：`D:\Temp\1crn.pdb`（327 atoms）应用出版卡通后显示白底平面飘带 + 配体球棍，
  表面默认隐藏；Debug 全量测试 190 test cases / 193,799 assertions 通过。
- 参考图与素材仅本地留存，不随仓库分发。
- 说明：参考图中的 2D 光谱/荧光衰减曲线属于数据绘图，不在 3D 渲染器内实现；
  3D 部分（轨道、出版卡通、MOF/多面体、干净出图）已用上述预设覆盖。

## 分子畸形、动态分辨率、网格面数与光追导出（v2.17）

**反馈**：轨迹里出现跨盒长键/环状“畸形”；需要可缩放渲染尺寸、动态分辨率、
可调分辨率、网格面数；光追默认在支持 GPU 的机器打开、无 GPU 默认关闭，并且
只用于图片渲染。

- **PBC 按键拓扑逐分量解卷**：`MoleculeStructure` 新增
  `unwrapAdjOffset`/`unwrapAdj`（CSR 键邻接）；`SceneStructure::registerStructure`
  构建邻接；`scene.h` 新增 `unwrapByComponents(ptsIn, pts, count, unwrapRoot,
  adjOffset, adj, box)`，对每个连通分量从根原子 BFS，子原子取相对父原子的
  `minimumImageDelta` 周期像。旧的“全部锚到最小索引原子”算法保留为无键拓扑
  回退。`applyTrajectoryFrame`、`buildWorldPositionsForFrame`、`fillInstanceSegment`
  均已切换。
- **长键安全网**：`Representer::bondAllowed` 在键长平方大于
  `(r_i + r_j + 1.25 Å)²` 时跳过该键。它不改变坐标，只避免异常 PBC 像被画成
  跨屏长键；正常共价键/配位键不受影响。
- **渲染尺寸/动态分辨率**（渲染视图顶部“渲染质量”）：
  `renderScale` 0.50–2.00×；勾选“动态分辨率”时，相机/坐标变化后的 350 ms 内
  内部 FBO 再乘 0.70（下限 0.35×），静止后恢复；界面图像仍按逻辑尺寸显示，
  相机宽高比与叠加层不受影响。默认 1.00×、动态开启。
- **网格面数**：低/中/高 → `OffscreenRenderer::setMeshQuality(detail, segments)`
  = (2,16)/(3,24)/(4,32)，实时重建；光栅化默认中，导出仍用高精度。
- **光追导出**：`Device::supportsGpuRayTracing()` 在设备创建时检查
  `VK_KHR_acceleration_structure` + `VK_KHR_ray_tracing_pipeline`。导出对话框
  默认勾选状态由 `MolScene::rayTraceSupported()` 决定：支持硬件光追才默认开，
  无光追默认关；无 GPU 的机器可手动勾选，走现有 CPU 光追回退。实时视口不再
  默认使用光追，光追只在导出图片时使用。
- **说明（重要）**：当前 `rayTraceSupported()` 只表示“设备暴露光追扩展”，
  渲染器本身仍没有 Vulkan 光追管线，导出实际走 `ray_tracer.cpp` 的 CPU 路径。
  因此文案明确写“CPU 回退”，不宣称已有 GPU 光追加速；后续若接 GPU 管线，
  只需替换 `exportRayTracedToPng` 的后端。
- **验证**：Debug/Release 190 test cases / 193,799 assertions 通过；
  `F:\MolecuLens\.monitor\polymer.gro` 加载后跨盒长键不再出现；快速导出
  `.monitor\rt_quick.png` 成功（1616×1364，242 KB，默认光追勾选状态随能力）。
- 已知残留：若用户文件仍出现畸形，需要该文件与具体帧；当前已覆盖 PBC 跨盒
  长键、近裁剪截面、lookAt 极点翻转三类已知原因。

## 应用图标重制（v2.18）

**反馈**：应用图标有白边；希望提升复杂度与辨识度，采用深蓝紫 + 白色搭配。

- 新图标主题：深蓝→靛蓝→紫罗兰渐变圆角方块；**不再使用白色外描边**，
  边缘保持深色，圆角外透明。
- 主体：冰紫罗兰色放大镜（圆环 + 45° 手柄）+ 白色中心原子/分子键 +
  淡紫苯环 + 青色轨道椭圆 + 少量星芒；深色底上白/冰蓝高对比。
- 分尺寸绘制，避免 16/24/32 px 缩小时糊成一团：
  - 16/24/32：粗放大镜轮廓 + 小手柄 + 青白样品原子，去掉苯环/轨道/星芒；
  - 48/64：放大镜 + 三原子分子；
  - 128–1024：完整苯环、轨道、星芒和镜片玻璃。
- 源脚本：`tools/make_app_icon.py`（Pillow/numpy，4096 超采样后缩放），
  输出 `design/logo/icon-1024.png`、`logo-512.png`、`size-*.png` 和
  `src/app/app.ico`（16/24/32/48/64/128/256 七档，PNG 压缩帧）。
- `src/app/CMakeLists.txt` / `src/app/app.rc` 未改路径，重新构建即嵌入新图标；
  `design/logo/app.ico` 同步更新。
- 验证：Debug/Release 重新链接成功；任务栏/窗口图标使用新深蓝紫放大镜，
  16–32 px 下仍能辨认“放大镜 + 样品”轮廓，无白色外圈。


## 左侧工具栏（v2.17）

**需求**：在工作区左侧加一条工具栏，放一些常用工具。

**做法**：在「左导航（92 px）」与「分子视口」之间插入一条独立的竖向工具条
（`src/app/workbench/style/tool_icons.cpp` + `Workbench::renderToolShelf`），
56 px 宽、可收起（Ctrl+T，视图 → 显示左侧工具栏）。为什么不并进左导航：
左导航回答“去哪个工作面”（渲染视图/分子/ESP/轨道/轨迹/分析/标注/日志/设置），
是**页面**；工具栏回答“现在用什么工具”，是**操作**。混在一起会互相误触。

工具分五组（图标全部 ImDrawList 矢量绘制，不引图标字体，任意 DPI 清晰）：

| 组 | 工具 | 说明 |
| --- | --- | --- |
| 交互（互斥） | 浏览 V / 测量 M / 标注 T / 箭头 A | 前四项同一时刻只有一个生效 |
| 视图 | 取景全部 F / 重置视角 R | 与视口悬浮条同一动作 |
| 显示（开关） | 坐标轴 / 比例尺 / 周期水盒 / 隐藏氢 H | 开关态：淡选中蓝底 + 实色图标 |
| 出图 | 导出 PNG Ctrl+E / 光追预览 / 浅色·深色 | — |
| 清理 | 清除全部距离测量 | 垃圾桶图标 |

**新增的交互工具**（`Workbench::handleToolClick`）：

- **测量**：依次点两个原子 → 视口内画虚线段 + `xx.xx Å` 标签；可连续测多组，
  日志同步 `[measure] O · EP #2 → C · EP #5: 9.67 Å`。
- **标注**：点中原子后在锚点旁弹输入框（预填 `O · EP #2` 这类原子描述），
  回车/「添加」写入 `labels_`，随 PNG 导出一起栅格化。
- **箭头**：点起点、再点终点 → 生成 `SceneArrow`（颜色取主题强调色），
  与「标注」页手动加的箭头完全同源（可导出、可保存进 .mlscene）。
- 拖拽仍交给相机（拿着尺子也能转视角），只有“按下→松开且位移 < 5 px”才拾取；
  Esc 取消中间态并回到浏览工具。

**拾取**（`Workbench::pickAtomAt`）：把每个原子投影到屏幕、按**像素距离**取最近
（阈值 `max(12 px, 字号)`），而不是复用 `PickRay::pickScene` 的 0.2–0.6 Å
世界半径 —— 后者在球棍/卡通下常常“看着点在球上却点空”。实测投影与
`projectToViewport` 完全一致（同一视口内两者 px 差 0.0）。

**踩到的坑（已修）**：

- 开关态原本用 `accentSoft` 底 + `accent` 图标。用户的强调色是浅粉时，
  变成“粉底粉图标”，浅色主题下几乎看不见（截图实测）。改用 `Palette::selection`
  （固定选中蓝，v2.14 已为导航/列表项引入）的 14% 淡底 + 实色图标。
- 视口左下“当前工具提示语”同样从 accent 改成固定选中蓝。
- 布局：`inspMaxByAspect` 同步减去工具栏宽度，否则窄窗口下视口被工具条挤成竖条。

**i18n**：`language.cpp` 新增 50 条工具栏文案（中英）；帮助框补 6 条快捷键。

**验证**：Release 构建通过；`EPI.pdb` 实测测量 9.67 Å（两点）、箭头 6.52 Å
（两点）、标注预填 `O · EP #2`、Ctrl+T 收起/展开、浅色主题下开关态图标清晰可辨。
