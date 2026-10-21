# MolecuLens · 缺陷修复与渲染优化记录（2026-10-18）

> 本轮目标：修复"不能渲染"的问题、修复相关 bug、优化渲染引擎。
> 环境：Windows 11 / MSVC 2022 19.44 / CMake 3.31.7 / Vulkan 1.4.357.0。

## 1. 核心缺陷：蛋白骨架类表示几乎完全不可见（"不能渲染"的根因）

### 症状

球棍 / 棍状 / 空间填充 / 表面 / QuickSurf 正常，但 **卡通（Cartoon）/ 管状（Tube）/
丝带（Ribbon）/ 新卡通（NewCartoon）** 在视口和离屏出图里几乎看不到：

| 测试结构 | 表示 | 修复前非背景像素 |
|---|---|---|
| 1UBQ | Cartoon | 45 |
| 1UBQ | Tube | 51 |
| 1UBQ | Ribbon | 11 |
| 1UBQ | NewCartoon | 124 |

整张 640×480 图只有中心几十个像素，骨骼缩成一团；导出的 OBJ 里 1UBQ 卡通
只有 **1 个网格对象 / 144 个顶点**（正常应是覆盖全链的数千顶点）。

### 根因

`src/io/pdb/pdb_parser.cpp` 用 **残基名** 作为残基索引的去重键：

```cpp
auto residueIdFor = [&](const std::string& name) {   // name = "GLY"/"ALA"/"SOL"…
    ...  // 同名残基共用同一个 residueId
};
const int32_t rid = residueIdFor(resName);           // ← 缺陷
```

PDB 里残基名只有 20 种左右，于是所有 GLY 共用 id 0、所有 ALA 共用 id 1、所有水共用
一个 id……而 `Atom::residueId` 的语义是"残基索引"，卡通 / 管状 / 丝带 / 新卡通
都按 `(chainId, residueId)` 分组提取骨架（`extractBackbones` / `extractCaChains`）。
分组后每个残基只剩"最后一个原子"，整条链被塌缩成十几个相距很远的点，再被
4.2 Å 断链阈值过滤 → 骨架几乎全部消失。1UBQ 有 76 个残基、却只得到个位数个点。

同源的次级缺陷：`same residue` 选择会选中整条链里所有同类残基；`resid` 选择
比较的是残基名编号而不是残基序号。

### 修复

- `src/io/pdb/pdb_parser.cpp`：残基去重键改为 **链 + 残基序号 + 插入码**
  `(chainID, resSeq, iCode)`，每个残基出现一次就 `addResidue(resName)` 分配唯一
  `residueId`（同时解析 `iCode` 列 27）。结构哈希 `computeStructureKey` 不含
  residueId，故不影响结构去重复用。
- 新增回归测试 `tests/io/pdb_parser_tests.cpp`（自造含重复残基名的 PDB）：
  断言 6 个残基得到 6 个不同 residueId、残基名可回查；管状生成 5 段圆柱 + 6 个端帽；
  丝带 5 段圆柱；卡通网格 X 跨度覆盖整条链。旧实现会直接失败。
- 兼容性提示：旧的 `.mlscene` 里保存的是旧语义的 residueId；含卡通/管状等表示的
  旧场景文件建议重新生成/重存。

## 2. 同一链路上的其它缺陷

| 文件 | 问题 | 修复 |
|---|---|---|
| `src/engine/style/cartoon/cartoon.cpp` | SS 判定用错原子（φ 用了前残基 N、ψ 用了下一残基的 φ），1UBQ 全判成 Coil，永远只有绿色管 | 按标准定义：φ=C(i‑1)‑N(i)‑CA(i)‑C(i)、ψ=N(i)‑CA(i)‑C(i)‑N(i+1)；现在螺旋红、折叠黄、卷曲绿 |
| 同上 | 三角带桥接用"当前环顶点数"反推上一环基址：Coil(8 顶点) 与 Helix/Sheet(2 顶点) 切换处下标错位、生成穿模三角形；断链处也会被桥接 | 显式保存上一环基址/顶点数，按 `max(prev,cur)` 取模桥接；断链（run 号变化）不桥接；标架退化时换参考轴；带面法线改用副法线（不再像"细棍"） |
| `src/cli/main.cpp` | CLI11 异常名写成 `Help/HelpAll`，实际是 `CallForHelp/CallForAllHelp` → `moleculens-cli --help` 不打印帮助、直接退出码 3 | 按 CLI11 2.5 的真实异常名匹配；`--help` / `render --help` 恢复输出、退出码 0 |
| `src/io/gro/gro_parser.cpp` | 日志"原子/键"按结构池统计，而结构池按 StructureKey 去重：md.gro 报成 210 原子（实际 6452） | 按实例累加：6452 原子 / 4436 键 |
| `src/cli/cmd/info/info_cmd.cpp` | 同上：`info md.gro` 只报首个实例（30 原子），且没有结构数 | 按实例累加并增加"结构数"：7 结构 / 2042 实例 / 6452 原子 / 4436 键 |
| `tools/build_incremental.bat`、`tools/build_rebuild.bat` | UTF‑8 中文注释在 cmd 的 OEM 代码页下被拆行，脚本报 `'d）。用法：build_inc.bat' is not recognized`，构建入口不可用 | 改为纯 ASCII + 明确注释，保留原 vcvars/MSBuild 命令 |

## 3. 渲染引擎优化：TriMesh 路径不再逐帧重建/拷贝

`src/engine/vulkan/draw/draw_geometry.cpp` 原实现里，**三角网格（表面 / 卡通 /
QuickSurf / 导入网格）无论命令流是否变化，每一帧都重新合并顶点、把材质色烘焙进
顶点色并重新上传**。QuickSurf 整体表面命令流约 19.7 MB，静止帧也要在 CPU 上白白
复制十几 MB。

现在：

- 合并、材质烘焙、上传只在命令流修订号（`listRevision` / `uploadRevision`）变化时执行；
- 按渲染模式（实心/线框/透明/点）把各段 `firstIndex/indexCount/pipeMode` 缓存进
  `FrameGpuBuffers::meshRanges`；未变帧直接按缓存段提交 `vkCmdDrawIndexed`；
- 顺带修掉旧实现"多渲染模式时同一网格缓冲被后一组覆盖"的隐患（现在一次性按模式
  顺序合并到同一缓冲，用 firstIndex 分段绘制）。

### 实测（Release，`data/md.gro` 6452 原子，`--rep quicksurf --system-surface`，1600×900）

| 指标 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| A 静止帧 | 9.70 ms（103 FPS） | **2.35 ms（425 FPS）** | **4.1×** |
| B 交互（相机） | 9.51 ms | 2.53 ms | 3.8× |
| 对照组：ballstick A 静止 | 1.33 ms | 1.38 ms | 无变化（噪声内） |

对照组证明提升来自网格路径优化，而不是编译/机器波动。

## 4. 验证结论

- **测试**：新构建树 `build_agent/`（Debug 与 Release）均为
  **135 用例 / 6987 断言全部通过**（原 4 个导出用例在此前工作区的失败，是旧
  `test_export.*` 残留文件不可写导致的环境问题，非代码缺陷；换可写工作目录后通过）。
- **CLI 帮助**：`--help` / `render --help` 正常输出并以 0 退出；无参数仍按约定返回 3。
- **离屏出图**（`moleculens-cli render 1UBQ --repr …`，640×480）像素覆盖提升：

  | 表示 | 修复前 | 修复后 | 倍数 |
  |---|---|---|---|
  | Cartoon | 45 | 6873 | 152.7× |
  | Tube | 51 | 6937 | 136.0× |
  | Ribbon | 11 | 2745 | 249.5× |
  | NewCartoon | 124 | 13712 | 110.6× |

  对比图见 `verification/ubq_cartoon_before_fix.png` 与
  `verification/ubq_cartoon_fixed.png`（另含 tube/ribbon/newcartoon 与 1HHO 卡通）。
- `moleculens-cli info data/md.gro`：6452 原子 / 4436 键（此前 30）。

## 5. 如何应用与构建

本会话的 Windows 沙箱对已存在的 `build/` 目录只有读权限（既有产物是高完整性
文件），无法原地重建；因此本轮在**工作区内新建构建树**验证：

```powershell
cmake -B F:/MolecuLens/build_agent -S F:/MolecuLens -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DVCPKG_INSTALLED_DIR=F:/MolecuLens/build/vcpkg_installed `
      -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF
cmake --build F:/MolecuLens/build_agent --config Release -j 8
```

可直接运行的修复后产物（Release）：

- GUI：`build_agent/src/app/Release/moleculens_app.exe`
- CLI：`build_agent/src/cli/Release/moleculens-cli.exe`
- 测试：`build_agent/tests/Release/moleculens_tests.exe`

在**普通（非沙箱）终端**里也可直接修复后的脚本增量重建原 `build/`：

```powershell
cmd /c F:\MolecuLens\tools\build_incremental.bat
```

## 6. 未包含在本轮的项

- QuickSurf 表面生成的**异步化**与 **GPU compute**（REMAINING_WORK.md 中优先级最高
  的 197 ms 主线程阻塞问题）——需要 GUI 线程模型与导出线程互斥重构，本轮未做。
- 阴影的 TriMesh 覆盖、限帧睡眠粒度等 REMAINING_WORK 小项。

## 7. 2026-10-18 追加：框架整洁化（清理项）

- 移除未实现的 Label（标签）表示与全部死代码（`drawLabelPlaceholder`/`LabelInst`/样式表槽位/
  序列化键）；`Cmd::Label=4` 协议槽位保留以兼容旧 `.mlscene` 二进制命令流。
- `surface.cpp` 私有 MC 四表去重到共享 `mc_tables.h`（−298 行，出图逐字节一致）。
- 删除根目录历史残留（Hello World `main.cpp`、`pkgconfig_stub.obj`、`test_export.*`）与
  迁移后的 CLI 兼容转发头、`tools/__pycache__`。
- 阴影贴图尺寸收敛为 `engine::kDefaultShadowMapSize`；主循环限帧改为
  `timeBeginPeriod(1)` + 自旋收尾（消除限帧睡眠粒度误差，拿回约 13 FPS）。
- 修复 C4189/C4267 两处警告 → Release 全量 **0 warning**；`.gitignore` 增补本地环境/新构建根。
- 验证：156/156 测试通过；ESP 表面出图 SHA256 与清理前一致。

## 8. 2026-10-18 追加：表面生成异步化（REMAINING_WORK §1.1 完成）

- 新模块 `src/engine/async/surface_jobs.{h,cpp}`：单工作线程 FIFO + 同键去重 +
  完成计数通知 + 导出可等待 + 幂等 shutdown。
- `quicksurf.cpp`：整体系 QuickSurf 计算抽为纯 CPU 的 `buildSystemQuickSurfMesh`
  （只吃数值拷贝，可跨线程）；异步模式下缓存未命中时提交任务并立即返回，
  结果写线程安全网格缓存。
- `Renderer`：新增 `setAsyncSurfaceEnabled / surfaceGenerationPending /
  prepareBlocking`；异步模式下面还没有就绪时**保留旧命令流**（旧画面不消失），
  任务完成后自动重建换入；`prepareMutex_` 串行化主线程与后台导出线程；导出路径
  （图像/光追/动画）改用 `prepareBlocking()` 等后台结果再同步重建。
- UI：视口左上角显示「表面生成中…」；GUI 初始化打开异步；CLI/测试默认同步不变。
- 实测（`md.gro` 6452 原子，`moleculens_bench --rep quicksurf --system-surface
  --async-surface`）：主线程 prepare **207.69 ms → 0.39 ms**（max 0.47），
  生成 87.8 ms 移到后台线程，UI 不再有 >50 ms 尖峰。
- 测试：新增 `tests/engine/async/surface_async_tests.cpp`（5 用例 22 断言，覆盖
  后台执行/去重/旧命令流保留/完成后换入/同步回退/导出阻塞）。全量 **161/161 通过**。
