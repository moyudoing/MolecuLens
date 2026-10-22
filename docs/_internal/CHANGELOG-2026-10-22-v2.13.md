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

## 验证

- Debug：187 test cases / 193,781 assertions 全通过。
- Release：187 / 193,781 全通过；`moleculens_app.exe`、`moleculens-cli.exe` 重建。
- 新增回归测试：`frozen system surface survives coordinate replay`（冻结重放保留 TriMesh、
  `meshSerial` 不变；解冻后全量重建并递增）、`prefetch jobs keep the normal queue idle`
  （低优先级预取不进入 `prepareBlocking` 的等待集合）。
- GUI 实测：逐帧 QuickSurf 播放 98–119 FPS；菱形/多面体按非氢原子渲染；ESP 色标位于
  时间轴上方；材质名称中文；选中行对比度已加强。

## 已知后续

- 视频导出（动画导出页签）需在下一轮验证/补齐；GPU 截图保存失败与窗口拖动闪烁另行处理。
- 分子放大后拖拽的形变、鼠标手势（左键方向、右键绕视平面法线）待处理。
