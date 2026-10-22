# MolecuLens v2.19 · 2026-10-22

> 本轮由 1 个 Lead + 4 个 teammate 并行完成（动态 FPS / 表面 / 作用分子范围 / GPU 光追）。
> 基线：v2.18（工作区，无 git）。验证：Debug/Release 均 **210 例 / 193,948 断言全过**。

## 1. 动态 FPS 轨迹播放 + 设置

**需求**：轨迹播放默认使用动态 FPS（跟随真实渲染帧率），并可在设置里调整。

- `FrameScheduler` 新增 `PlaybackMode{RefreshSync, FixedFps, Dynamic}`：
  - Dynamic：维护渲染 dt 的 EMA（α=0.2，首帧取 dt），播放间隔
    `clamp(max(smoothDt, 1/目标帧率), 1/maxFps, 1/minFps)`（默认 10–144 FPS）；
    每个渲染帧最多推进 `maxCatchUp`（1–8，默认 2）帧，超出预算**丢弃**而不积 backlog；
  - `speed` 倍率保留；切模式/改固定帧率清零 accumulator；
  - 新增 `effectiveFps()`（UI 显示实际轨迹帧率）。
- `TimelineController` 构造时 `applyPlaybackSettings()` 从 settings.json 读模式/目标/倍速/追帧；
- `settings.json` 新增顶层键：`playbackMode("dynamic"/"fixed"/"refresh")`、`playbackFps`、
  `playbackSpeed`、`playbackMaxCatchUp`（缺键默认 dynamic/60/1.0/2，旧 `timeline.speed` 回退）。
- UI：轨迹模块「播放帧率」首项改「动态 FPS」（另有刷新同步/12/20/24/30/60/120），
  状态行显示「动态 × 1.00 · 目标 60 FPS · 实际 60 FPS」；设置页新增「轨迹播放」卡片
  （动态/固定/同步 + 目标帧率 0–144 + 追帧上限 + 倍速）。
- GUI 实测：md.gro+md.xtc（1001 帧），点播放 3 s 从第 2 帧到第 202 帧（≈66 FPS），
  状态行显示「实际 60 FPS」（界面 117 FPS，目标 60）；settings.json 已落盘四个键。

## 2. 分子表面的绘图模式（崩溃 + 着色 + 线框/半透明）

**根因**：`Renderer::prepare` 的「整体系合并表面」分支构造 `RepContext` 时**没有 instance**，
`drawSystemSurface` 对每个原子调用 `Representer::colorForAtom(ctx,g)` → 读
`ctx.instance->structureIndex` → **空指针崩溃**。CLI `--system-surface` 必崩；GUI 里新建
「分子表面」（默认整体系合并）会直接崩溃。

- `Representer::colorForAtomAt(ctx, structureIndex, localAtomIndex, instanceIndex)`：不依赖
  `ctx.instance` 的取色核心，`colorForAtom` 复用它（旧行为逐值保持）；
- `drawSystemSurface` / `buildSystemSurfaceInput`（QuickSurf）改用它：整体系表面按原子
  真实所属实例逐顶点着色（元素/链/残基/统一色都生效），并消费实例范围掩码；
- 新增 `Representation::surfaceDrawMode`（0 实心 / 1 线框 / 2 半透明）：逐分子路径由
  `Representer::generate` 覆盖 RenderMode，整体系路径由渲染器发射对应 RenderMode
  （此前被强制 mode 0，用户没有可用的表面绘图模式）；
- 「表面」面板改为编辑**当前选中**的 Surface/QuickSurf（不再永远第一条），新增
  「绘制模式」下拉（实心/线框/半透明）。
- CLI 实测：`--rep surface:all:name:1.0 --system-surface` exit 0、PNG 22 KB，连续表面、
  C 青/O 红/H 灰逐原子着色；GUI 实测：新建「分子表面」不再崩溃，切换到「线框」后
  视口立即变为线框网格。
- 回归测试：2 实例 systemSurface 不崩溃 + TriMesh + 逐顶点色非全同；drawMode 0/1/2 →
  RenderMode 0/1/2。

## 3. 作用分子选择优化 + 分子类型范围

**需求**：优化「作用分子」选择；新增按类型选择实例范围的设计（例如只渲染一半溶剂）。

- `Representation` 新增实例级范围字段：`rangeMode`（全部/按比例/每隔 N 个）、`rangeType`
  （类型名，如 SOL）、`rangeFraction`、`rangeStride`、`rangeOffset`；`RepContext::instanceMask`；
  `buildInstanceRangeMask()`（renderer.cpp，纯函数、确定性：类型 token 取实例名 `#` 前并
  trim；Fraction 按匹配实例顺序取前 `ceil(f*N)`；Stride 取 `p%N==offset%N`）；
- 渲染器每条表示算一次掩码：逐实例路径跳过；整体系表面（surface/quicksurf）同样消费；
- 持久化：`.mlscene` 快照 + `scene_serializer.cpp` + `scene_bridge.cpp`（缺字段默认值，
  旧文件向后兼容）；
- UI：作用分子编辑器重做——类型分组、搜索框、每类型「实例数/每实例原子/总原子」、
  全部分子/仅非溶剂/清空、表达式预校验（非法红字且不覆盖旧值，合法显示「已应用 · 命中 N 原子」）、
  新增「类型范围」块（类型下拉、全部/比例/步长、25/50/75/100% 快捷、步长 N/偏移 M、
  预览「将渲染 X / Y 个 SOL 实例（共 N 原子）」；表达式选原子、类型范围选实例，取交集）；
- GUI 实测：md.gro（2042 实例）作用分子编辑器显示 API/EPI/NAR/R/Q/T/SOL 分组与计数
  （SOL ×2032 · 3 原子/实例 · 共 6096 原子），类型范围块与预览文字正常。
- 测试：`instance_range_tests.cpp`（7 例：4 SOL+2 PROA 的 fraction/stride/空类型/0/1 边界/
  顺序确定性 + Renderer 集成 SphereInst 计数）+ scene_bridge 往返/旧文件缺键。

## 4. 鼠标手势不可用（修复）

**根因**：
1. ImGui 鼠标键编号是 0=左 / 1=右 / 2=中，旧代码按「中=1、右=2」写分支 → **右键实际
   在 dolly、中键在滚转**，与面板/文档（右键滚转、中键推拉）相反；
2. 拖拽锁存「按下即锁定」：先点一下左键（无位移）再按右键拖动，右键事件被旧
   `dragButton_` 拒绝 → 看起来「右键/中键手势完全用不上」。

**修复**（`mol_scene.cpp::handleMouse`）：按 `ImGuiMouseButton_*` 常量分支
（左=轨道/右=滚转、Shift+右=平移/中=dolly）；只有真正产生位移才进入拖拽；每帧兜底
在「拖拽键已抬起（即使别的键还按着）」时解锁；滚轮在画面悬浮时始终生效。

> 说明：代码层修复已完成并通过构建；GUI 目视复验因桌面锁屏未能在本轮完成，解锁后
> 按「左键旋转 / 右键滚转 / Shift+右键平移 / 中键推拉 / 滚轮缩放」五项各做一次即可。

## 5. 完整 GPU 光追模块（Vulkan KHR）

**新增**：`src/engine/vulkan/raytracing/gpu_ray_tracer.{h,cpp}`（BLAS/TLAS、5 组光追管线、
SBT、描述符、提交与 RGBA8 回读）；着色器 `shaders/rt_{raygen.rgen,miss.rmiss,shadow.rmiss,tri.rchit,prim.rint}`
+ `rt_common.glsl`；`cmake/shaders.cmake` 按扩展名自动 `--target-env vulkan1.2`。

- 设备层真正启用 `VK_KHR_acceleration_structure` / `VK_KHR_ray_tracing_pipeline` /
  `VK_KHR_deferred_host_operations` / `VK_KHR_buffer_device_address` + feature 链，
  查询 RT/AS 属性；创建失败自动退回无 RT 设备；headless 设备不再启用 swapchain
  （消除 VUID-01387）；
- 几何：三角网格走三角形 BLAS；球/胶囊用 AABB 程序化几何 + intersection shader 解析求交
  （保持解析精度）；光照与 CPU 路径同构（半球环境光 + 方向光 Blinn-Phong + 软阴影），
  opacity<1 做多层前向 alpha 合成（≤4 层）；
- 接入：`Renderer::gpuRayTraceAvailable()/gpuRayTrace()`（懒初始化、持 `lockQueue()`）；
  `MolScene::rayTraceSupported()/exportRayTracedToPng()` 优先 GPU、失败回退 CPU；
  CLI 新增 `--rt-backend cpu|gpu|auto`（auto 有 RT 走 GPU，无 RT 自动 CPU）；
- **关键排障**：所有射线「全 miss」的根因是 miss/closest-hit 用了 `rayPayloadEXT`
  （出向 RayPayloadKHR），写入落不到 raygen 的入向载荷；改为 `rayPayloadInEXT` 后正常；
- 验证：
  - `rt_smoke`（1 三角 + 1 球，64×64）→ 15 assertions 全过，
    `[rt] gpu: 1 tris / 1 prims, BLAS 1.8 ms, TLAS 0.4 ms, trace 2.9 ms`；
  - CLI GPU vs CPU（EPI.pdb ballstick，2 spp，480×360）：nonBg 8786 vs 8787，
    掩膜 IoU 0.9999，全图 MAE 0.124/255；
  - `--rt-backend auto/gpu/cpu` 日志 backend 正确，auto 与 gpu 逐字节一致；
  - `MOLECULENS_RT_VALIDATE=1` 下无任何 VUID。
- 已知差异：GPU 不做 CPU 的 0.18 弱反射（入向载荷不能再当嵌套 trace 载荷；
  实测 MAE 0.12/255），透明层上限 4（CPU 8），不影响轮廓/验收。

## 6. 工程与协作

- 新增 `tools/agent_build.ps1`：多 agent 共享构建树时的**跨进程文件锁**构建包装器
  （`-Configure` 在锁内先跑 cmake，改着色器后必须用它；exit 0 才表示产物已更新）。
- 本轮四个 teammate 的写作用域见 `.tmp/team-plan-dynamic-rt.md`；`modules.cpp` 三处
  并发编辑均用唯一锚点外科式修改。
- 验证结论：Debug 与 Release 各 210 例 / 193,948 断言全过；Release app/cli/tests 均重建成功。
