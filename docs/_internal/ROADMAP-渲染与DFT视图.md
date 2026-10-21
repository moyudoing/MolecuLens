# MolecuLens · 渲染与 DFT 视图路线（只保留未落地项）

> 快照：2026-10-22 02:05（Asia/Shanghai），代码基线 v2.11。
> 范围：**尚未落地**的方向、验收标准与选型。已完成的实施记录（视锥剔除、QuickSurf AVX2、BVH 光追、
> 三角形光追、RT LOD、玻璃观感）连同全部实测数字已归档：
> [`archive/ROADMAP-渲染与DFT视图-20261022-全量.md`](archive/ROADMAP-渲染与DFT视图-20261022-全量.md)。
> 现存缺陷见 [`ISSUES-已知问题登记.md`](ISSUES-已知问题登记.md)；环境与构建约定见 [`../../REMAINING_WORK.md`](../../REMAINING_WORK.md)。

---

## 0. 结论先行

| 方向 | 现状 | 下一步（按性价比） | 验收 |
|---|---|---|---|
| 渲染性能 | 实例化 + 视锥剔除 + 120 FPS 限帧；6.4k 原子场景瓶颈在**片元填充/呈现** | 先量填充率与超大体系才谈优化；细分档仍为静态两档 | 缩小场景顶点量降 ≥50% 且无观感差异 |
| 密度场 | AVX2 + 多线程 MC（Release 166→49 ms，3.4×） | GPU compute（`surface.comp` 已写、缺 `.cpp` 与构建接入） | 同场景 QuickSurf 再降一个量级，且与 CPU 路径视觉一致 |
| 光线追踪 | 自研 BVH（球/胶囊/三角形，231–310×）+ RT 预览两级 LOD | 表面不透明度、AO/GGX、降噪；非 GUI 导出复用 BVH | 同 spp 噪点明显下降，接近 VMD Tachyon 观感 |
| DFT 属性 | cube 解析 + ESP 等值面 + 9 种色标 + 色标条 | **MO 等值面 + 能级图**（第一优先），再 DOS/PDOS、CDD/自旋 | 一组 cube 能直接出一张"轨道 + 能级"论文插图 |

---

## 1. 现状基线（已具备，设计新功能时复用）

- **表示**：16 种（`RepresentationType`：Wireframe … Plate / Diamond / DotCloud），球/键 GPU 实例化。
- **命令流**：`cmd/command_list.cpp` + serial 缓存；几何未变时跳过重上传；单命令 16 MiB 上限由 `appendTriMesh` 切块。
- **键搜索**：`entity/search/bond_search.cpp` cell-list + 按 box 切片多线程。
- **表面**：`style/surface`（整体系 vdW MC）、`quicksurf`（AVX2 + 按 z 分层多线程）、`espsurface`（密度等值面 + 势逐顶点着色 + 色标条）。
- **剔除**：`draw_geometry.cpp` 用 6 个视锥平面过滤实例/键包围球，实时与离屏都生效；**默认常开，没有 `--cull` 开关**（曾计划加，未做，见 §2.9）。
- **光追**：`raytrace/ray_tracer.cpp` 自研 BVH（质心中位数二分、叶 4、并行建树），球/胶囊解析求交 + 三角形 Möller–Trumbore；`--rt N`、`MOLECULENS_RT_NO_BVH=1`、`MOLECULENS_RT_THREADS=N`。
- **RT 预览**：`MolScene` 缓存 `rtSceneCache_/rtAccelCache_`（键 = `commandSerial()`），交互档 320/1spp、停手 0.25 s 升到 720/2spp；纹理固定按高档尺寸，避免重建描述符。
- **导出**：1×/2×/4×、SSAA、16-bit、透明背景、标注栅格化；质量档预览 3/24、导出 4/48。
- **DFT 输入**：Gaussian cube（密度/势/轨道），`docs/05-DFT常用绘图指南.md` 已有 22 行「图类型 → 数据来源 → 参数 → 工具 → 现状」对照。

---

## 2. 开放项

### 2.1 自适应 LOD（P1，2–3 天）
- **现状**：细分档是静态两档（预览 detail 3 / 键 24 段；导出 4 / 48 段），与相机距离无关。
- **做法**：按投影后像素直径 `d_px` 选档：`d_px < 6` → detail 1–2；`6–24` → 3；`>24` → 4（导出固定 4）。需要在小网格集合里再加 1–2 档，把"全局档位"改成"每批按 `d_px` 选缓冲"。
- **验收**：缩小看全场景时顶点提交降 ≥50%，拉近观感无差别；导出图与当前逐字节一致（导出路径不参与）。

### 2.2 MO 等值面 + 能级图（P1，1 周）
- **缺口**：cube 已是 `(x,y,z,value)`，但没有"正负相位双色 + 多轨道叠加 + HOMO/LUMO 能级图"。
- **做法**：复用现有 MC；按 value 符号取两色（正/负相位），轨道多选（同目录多个 cube 或同一 cube 组）；能级图用 ImGui 自绘（能级、占据数、gap 标注）。
- **参考**：OrbVista（Molden/FCHK → MO/ESP/自旋密度）、Multiwfn 的轨道输出约定。
- **验收**：一个 cube 组能出"MO 等值面 + 能级图"插图并导出 PNG，`docs/05` 支持度对照表同步加行。

### 2.3 DOS / PDOS（P2，3–5 天）
- **缺口**：无。**做法**：读 `vasprun.xml` / `.dos` / Multiwfn 导出文本 → 曲线面板 + CSV 导出。
- **参考**：pymatgen `Dos`/`CompleteDos` 的解析规则（只借格式，不引运行时依赖）。
- **验收**：一个 VASP 算例能画总 DOS + 指定原子 PDOS，可导出 PNG/CSV。

### 2.4 CDD / 自旋密度（P2，各 3–5 天）
- **做法**：新增 cube 运算 `ρ(AB) − ρ(A) − ρ(B)`（自旋密度为 α−β），等值面 ±0.005，复用 MC + 正负双色。
- **验收**：三个 cube 输入直接出 CDD 图；与 Multiwfn 同参数结果等值面形状一致（目视 + 包围盒尺寸对比）。

### 2.5 其余 DFT 图类型（P2/P3）
NCI / RDG（先支持显示外部算好的 sign(λ₂)ρ cube）、能带结构（`vasprun.xml` / `EIGENVAL` → k 路径 + 带隙）、PES/IRC 能量曲线（Gaussian/ORCA 输出，cclib 可做参考解析）。每项独立提交，各自带一个可导出 PNG 的面板。

### 2.6 RT 观感与工程化（P2，1–2 周）
- **表面不透明度**：RT 目前按不透明着色，与 GPU 半透明不一致（ISSUES I-25）。
- **材质**：GGX 高光 + 菲涅尔 + 环境光遮蔽（AO），参数对齐 VMD Tachyon；加简易降噪（双边滤波）或 2× 超采样。
- **BVH 缓存**：GUI 预览已有；CLI/批量导出每次重建（Release 911k 三角形约 118 ms），可做成跨调用缓存。
- **验收**：同 spp 下噪点明显下降；与 VMD 同结构同角度截图观感接近；导出时间不劣化。

### 2.7 GPU 密度核（P2，工作量最大）
- **已就绪**：`shaders/surface.comp`（6486 B）、`src/engine/vulkan/compute/surface_compute.h`（push constant 128B、SSBO、`compute()` 签名）。
- **缺**：`surface_compute.cpp`；`shaders/CMakeLists.txt` 未注册 `surface.comp`；全仓库 `vkCmdDispatch` / `vkCreateComputePipelines` 出现次数为 0。
- **做法**：参照 `vulkan/shadow/shadow_map.cpp`（自带管线的独立 pass）建描述符布局/池 + SSBO + dispatch + 屏障；队列提交持 `VulkanDevice::lockQueue()`；不支持或失败回退 CPU（`quickSurfSetBackend` 已有开关）。
- **验收**：`--rep quicksurf --system-surface` 构建命令流耗时显著下降，出图与 CPU 路径一致（允许阈值内像素差，需先定标准）。

### 2.8 光追路线选型（若自研 BVH 不够用再引入）
| 方案 | 类型 | 适配度 | 备注 |
|---|---|---|---|
| 自研 BVH（现状） | CPU | ★★★★★ | 已 231–310×，球体继续走解析求交更快 |
| [tinybvh](https://github.com/jbikker/tinybvh) | header-only BVH | ★★★★ | 零依赖；只在需要 BVH8/GPU 版时替换 |
| [Vulkan RT](https://github.com/AlexanderVeselov/RayTracing) | GPU 硬件光追 | ★★★ | 需 RTX/RDNA2+；实时预览级 |
| OSPRay / Embree | 生产级 CPU RT | ★★★ | 打包体积与许可成本高，非必要不引入 |

### 2.9 性能剩余项
- **剔除**：实现正确，但 6.4k 原子场景无可测收益（放大后片元填充成瓶颈）。要到 10⁵ 原子级体系、或多视口/平移浏览时再回来量实际 draw 数（需要加计数打点），并补 `--cull 0/1` 做 A/B。
- **填充率**：若用户仍感觉卡，先看窗口是否远大于 1366×768、是否开了 SSAO/阴影/线框叠加；限帧器目标 120 FPS 已实现高精度睡眠。

---

## 3. 数据层建议（做 2.2–2.5 之前先统一）

- 引入 `PropertyField {dims, origin, spacing, values, sign convention}`：密度、势、轨道、CDD、自旋都进这个结构，渲染层只认它 → 新增属性图 = 新增"怎么算/怎么读"的适配器。
- 体积数据内存：cube 512³ ≈ 537 MB → 需要分块/降采样（参考 VMD `VolumetricData`）。
- 解析层用 **cclib**（Python）做参考验证，但**不引入运行时依赖**：C++ 只需覆盖 Gaussian/ORCA/VASP 常见片段。

---

## 4. 不要重做（已被实测否定）

1. **共享单位球 + 逐实例变换**：surface 的 911,096 三角形是整体系 MC 网格（约 141 三角形/原子），不是逐分子球面；要减三角形只能降精度或网格简化。
2. **BVH 调参**：叶子 4→16、并行阈值 8192→2048、线程预算 8→16 均更慢，已回退（瓶颈是内存带宽）。
3. **逐实例 QuickSurf 缓存**：2.1× 但栅格相位差 21.9%，已回退；是否接受仍在 ISSUES I-17 待决。
4. **视锥剔除的即时收益**：6.4k 原子场景无收益，别再用它解释"变快/变慢"。

---

## 5. 约束

1. **逐位一致是验收线**：改渲染路径前拍基线 PNG md5，改完必须完全相同；只有明确声明的视觉改动才允许差异（需同步重出插图）。
2. **回归**：每次改动后跑 `moleculens_tests`（最近记录 182 例 / 193,749 断言，只增不减）；构建三件套 app/cli/tests 都要 exit 0。
3. **依赖纪律**：优先 header-only；引入 OSPRay/Embree 需评估打包体积与许可。
4. **GUI 验收依赖解锁桌面**；锁屏时只能走 CLI 离屏。
5. **文档同步**：新增图类型必须更新 `docs/05` 的支持度对照表并重出 DOCX（见 ISSUES I-14）。
