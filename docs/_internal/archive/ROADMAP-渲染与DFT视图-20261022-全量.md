# MolecuLens v2 · 渲染与 DFT 视图优化方案（含开源选型）

> 版本：2026-10-21 17:10（Asia/Shanghai）
> 目标：把「渲染性能 / 光线追踪 / DFT 属性展示」三块拉成一条可执行的路线，并给出每个方向**可借鉴的开源实现**与落地改造点。
> 前置：`ISSUES-已知问题登记.md`（现存缺陷）、`HANDOFF-新会话接手-20261021-1240.md`（工程现状）。

---

## 0. 结论先行

| 方向 | 现状 | 最高性价比动作 | 预期收益 |
|---|---|---|---|
| 渲染性能 | Vulkan GPU 实例化；键搜索已是 cell-list；quicksurf 已多线程 | **视锥剔除 + 屏幕尺寸自适应细分（LOD）** | 缩小看全场景时 draw/顶点量降一个数量级 |
| 密度场生成 | CPU 多线程高斯 splat（对标 QuickSurf 的标量版） | **AVX2 内核**（成熟工程范式） | 单核 3–4×，叠加多线程总提速 2–3× |
| 光线追踪 | 已有 CPU 光追（`src/engine/raytrace/`），未见 BVH | **tinybvh 加速 + 简单降噪** | 同质量下 5–20×；或同时间内可加 2–3 次反弹 |
| DFT 属性 | cube 解析 + ESP 表面 + 色标 | **MO 等值面 + 能级图 + DOS/PDOS**（数据侧接 cclib/pymatgen） | 从"能看 ESP"到"能出一套论文插图" |

---

## 1. 现状盘点（改之前先认清有什么）

**已有能力**
- 14 种表示（`src/engine/style/*`），球/键 GPU 实例化（`draw_geometry.cpp`）。
- 场景命令流 + serial 缓存（`src/engine/cmd/command_list.cpp`），几何未变时跳过重上传。
- 键搜索：`src/entity/search/bond_search.cpp` —— **cell-list + 按 box 切片多线程**（与 LAMMPS `npair bin` 同思路，这块不缺）。
- 表面/等值面：`src/engine/style/surface`、`quicksurf`、`espsurface` —— 高斯密度网格 + marching cubes（`quicksurf.cpp` 用 `std::thread` 按 z 分块）。
- 光追：`src/engine/raytrace/ray_tracer.cpp`（球体解析求交 + 软阴影，见 exportToPng 的 RT 分支）。
- 导出：1×/2×/4×（1920×1080 基准）、SSAA、16-bit、透明背景、标注栅格化。
- 质量档：预览 3/24（球 1280 面、键 24 段），导出 4/48（5120 面、48 段）。

**明确缺口**
1. 无视锥剔除：缩小/移出视野的实例仍然全量提交（搜 `cull|frustum` 无实现）。
2. 细分档是**静态**的（预览/导出两档），不是按屏幕投影尺寸自适应。
3. 密度核是标量循环，没有 SIMD；已有成熟实现提供 AVX2/NEON 专用版本。
4. 光追没有加速结构（BVH），复杂场景退化为逐个求交。
5. DFT 侧只有 cube → ESP 表面；没有 MO/DOS/自旋密度/CDD/能带。

---

## 2. 渲染性能：从成熟实现 / LAMMPS 学什么

### 2.1 可借鉴的工程实现

| 参考做法 | 关键点 | MolecuLens 对应 | 改造建议 |
|---|---|---|---|
| 高斯密度核的 SIMD 版本（AVX2 / NEON） | 密度核按 8 个格点向量化 | `quicksurf.cpp` 标量三重循环 | 抽出 `splatRow(...)` 用 `_mm256_fmadd_ps` 一次算 8 个格点；本机若支持 FMA，单核可 3–4× |
| 密度线程 + 归约线程两段式 | 分块计算与归约流水线重叠 | 已有 `std::thread` 分块 | 补"归约与 splat 流水线重叠"，减少串行尾部 |
| 显示列表缓存（命令流只重建变化部分） | `DrawMolecule` 的 display list | 已有 serial 缓存 | 可进一步做**按表示粒度**的脏标记，避免整段重建 |
| 分辨率随距离缩放 | 各 rep 的 `resolution` 参数 | 现在是静态档位 | 见 2.2 LOD |

> 成熟实现的密度核约 60 KB，一半是 SIMD 分支和线程同步，**算法本身和我们一致**，值得学的是工程实现而不是思路。

### 2.2 具体改造点（按优先级）

1. **视锥剔除（P0，1–2 天）**
   - 位置：`src/engine/vulkan/backend/draw_list.cpp`（生成 draw runs 前）或 `draw_geometry.cpp`；
   - 做法：相机 view-proj 矩阵取 6 个平面，对每个实例的**包围球**（中心 `pos`、半径 `radius*radiusScale`）做测试，不可见则不进 run；
   - 收益：md.gro 2042 分子在放大到局部时，提交量可降 90%+；命令行 `--cull 0/1` 便于 A/B；
   - 风险：低（纯 CPU 过滤，不影响几何）。

2. **自适应 LOD（P1，2–3 天）**
   - 用投影后的像素直径 `d_px` 选档：`d_px < 6` → 球 detail 1/2；`6–24` → 3；`>24` → 4（导出固定 4）；
   - 与现有 `setMeshQuality` 打通：把"全局档位"改成"每批按 d_px 选缓冲"（需要在小网格集合里再加 1–2 档，成本 = 几份小 VBO）；
   - 收益：缩小看时不浪费三角形；拉近时不糊。

3. **密度核 SIMD + 归约优化（P1，2 天）**
   - 依据：成熟实现的 AVX2 密度核；
   - 注意：MSVC 需 `/arch:AVX2` 或函数级 `__declspec(cpu_specific)` + 运行时分派（`__cpuid` 判断），否则老 CPU 崩；
   - 验收：quicksurf 生成时间对比（md.gro，1200×800），期望 ≥2×。

4. **（可选）GPU 密度核（P2）**
   - 用 Vulkan compute 做 splat + marching cubes；已有成熟实现提供 CUDA 版本可作参考架构；
   - 收益大但工作量大，建议等 P0/P1 稳定后再评估。

### 2.3 LAMMPS 的可借鉴点

| LAMMPS 机制 | 文件 | 可迁移的点 |
|---|---|---|
| 邻居表 + skin 距离 | `src/neighbor.cpp` | 键搜索已有 cell-list，但可加 **skin**：盒子变动小于阈值时复用上一次的候选对，避免每帧重建 |
| `npair` 分箱半邻接表 | `src/npair_half_bin_atomonly_*.cpp` | 键对只算 `j>i` 的一半，避免重复；确认 `BondSearch` 是否已去重 |
| SoA 内存布局 + 紧凑遍历 | `src/atom.h`、`src/domain.cpp` | 把 `Atom` 的 element/chain/residue 拆成并行数组，缓存友好（影响大场景 CPU 遍历） |
| OpenMP 分块归约 | 多处 `#pragma omp` | 我们已用 `std::thread`，可补 `std::execution::par` 或任务队列 |

> 结论：**LAMMPS 的"分箱 + skin + 半邻接"是最值得抄的**，键搜索已经走了一半（分箱有了，skin 和去重待补）。

---

## 3. 光线追踪：选型与路线

### 3.1 现状

- 已有 CPU 光追：`src/engine/raytrace/ray_tracer.cpp`，实现"球体解析求交 + 多次软阴影采样"，输出走 `exportRayTracedToPng`。
- 缺：BVH/加速结构（大场景 O(N) 求交）、材质模型（现在只有 lambert+高光）、降噪、GPU 加速。

### 3.2 可选开源方案对比

| 方案 | 类型 | 优点 | 代价 | 适配度 |
|---|---|---|---|---|
| **[tinybvh](https://github.com/jbikker/tinybvh)** | header-only BVH（CPU/GPU） | 单头文件、零依赖、构建快（SAH 近似）、支持 BVH8 | 需要自己写遍历与材质 | ★★★★★ 最适合先落地 |
| **[OSPRay](https://www.khronos.org/assets/uploads/developers/presentations/Portable_and_Scalable_Rendering_with_ANARI.pdf)**（经 ANARI 调） | CPU/GPU 生产级光线追踪 | 科学可视化成熟、支持体积 | 依赖重（oneAPI 运行时），打包体积大 | ★★★☆ 适合"要出版级图" |
| **[Vulkan RT (VK_KHR_ray_tracing_pipeline)](https://github.com/AlexanderVeselov/RayTracing)** | GPU 硬件光追 | 实时预览可用、与现有 Vulkan 后端同栈 | 需 RTX/RDNA2+ 硬件、着色器要写 RT 管线 | ★★★★（有硬件时） |
| **Intel Embree** | CPU BVH | 快、成熟 | 引入二进制依赖与许可管理 | ★★★ |
| **Tachyon** | 参考实现 | 分子领域经典、参数直观 | 只作参考（GPL 混合） | 参考 |

### 3.3 推荐路线（分三步）

**Step 1（1 周）：tinybvh + 现有 RT**
- 用 tinybvh 构建三角/球体 BVH，替换 `ray_tracer.cpp` 的线性求交；
- 保留现有材质与采样策略，只换加速结构；
- 验收：md.gro（6452 原子，球棍）1024×768、64 spp 的导出时间，对比现状；目标 ≥5×。

**Step 2（1–2 周）：材质/采样升级**
- 加 GGX 高光 + 菲涅尔 + 环境光遮蔽（AO），参考 Tachyon 与环境光遮蔽参数；
- 加简易降噪（双边滤波）或 2× 超采样；
- 验收：同 spp 下噪点明显下降，出图接近 Tachyon 观感。

**Step 3（可选，2–3 周）：Vulkan RT 预览**
- 有 RT 硬件时做"实时软阴影/AO"预览通道；没有则跳过；
- 参考实现：[AlexanderVeselov/RayTracing](https://github.com/AlexanderVeselov/RayTracing)（Vulkan/D3D12 路径追踪骨架）。

> 注意：OpenGL/Vulkan 的球体可以继续用解析求交（比三角 BVH 更快），BVH 主要加速**表面网格**（quicksurf/ESP）与**阴影射线**。

---

## 4. DFT 属性视图

### 4.1 现状

- `src/io/cube/cube_parser.cpp`：读 Gaussian cube（密度/势/轨道），`BondSearch` 推断键。
- `espsurface`：密度等值面 + 势场着色 + 9 种色标 + 色标条叠加。
- 文档 `docs/05-DFT常用绘图指南.md` 已把「图类型 → 数据来源 → 参数 → 工具 → MolecuLens 现状」列全（22 行支持度对照）。

### 4.2 缺口与建议（按论文插图的使用频率排序）

| 图类型 | 缺什么 | 建议实现 | 开源参考 |
|---|---|---|---|
| **MO 等值面 + 能级图** | 轨道多选、正负相位双色、HOMO/LUMO gap 能级图 | cube 已是 `(x,y,z,value)`，加"按符号双色 + 多轨道叠加"，能级图用 ImGui 自绘 | [OrbVista](https://github.com/ts-quantum/OrbVista)（Molden/FCHK → MO/ESP/自旋密度）、[MolStudio](https://chemrxiv.org/doi/full/10.26434/chemrxiv.15009253/v1) |
| **DOS / PDOS** | 无 | 读 `vasprun.xml` / `.dos` / Multiwfn 导出文本 → 画曲线（ImGui 或导出 CSV） | pymatgen 的 `Dos`/`CompleteDos` 解析较完整，可只借格式解析规则 |
| **电荷密度差 CDD** | 需要三个 cube 相减 | 新增"cube 运算"：`ρ(AB) − ρ(A) − ρ(B)`，等值面 ±0.005 | 现成方法见 `docs/05` §CDD；实现上复用现有 marching cubes |
| **自旋密度** | 同上（α−β） | 复用 cube 运算 + 正负双色 | 同上 |
| **NCI / RDG / IGMH** | 需要梯度、散点图 | 先做"显示外部算好的 sign(λ₂)ρ cube"，再考虑积分散点 | Multiwfn 的方法 + NCIPLOT 的输出约定 |
| **能带结构** | 无（周期体系） | 读 `vasprun.xml`/`EIGENVAL`，画 k 路径 + 带隙 | pymatgen `BandStructure` |
| **PES / IRC 曲线** | 无 | 读能量扫描输出（Gaussian/ORCA），画能量曲线 | cclib 可解析 Gaussian/ORCA 输出 |

### 4.3 数据层建议

- 统一一个 `PropertyField` 抽象：`{grid dims, origin, spacing, values, sign convention}`；
- 所有 cube 类数据（密度、势、轨道、CDD、自旋）都进这个结构，渲染层只认它 → 新增属性图 = 新增一个"怎么算/怎么读"的适配器；
- 解析层用 **cclib**（Python）做参考实现验证，但**不引入运行时依赖**（我们的 C++ 只需覆盖 Gaussian/ORCA/VASP 的常见片段）；
- 体积数据要考虑内存：cube 512³ ≈ 134M float ≈ 537 MB → 需要分块/降采样（成熟体数据实现的降采样策略可参考）。

---

## 5. 实施路线（建议按此排期）

| 阶段 | 内容 | 验收标准 | 预估 |
|---|---|---|---|
| **P0** | 视锥剔除 + `--cull` 开关 | md.gro 放大时 draw 实例数降 ≥80%，帧时间不变差 | 1–2 天 |
| **P0** | 密度核 SIMD（AVX2 + 运行时派发） | quicksurf 生成时间 ≥2× | 2 天 |
| **P1** | 自适应 LOD（按像素直径选档） | 缩小场景顶点量降 ≥50%，观感无差别 | 2–3 天 |
| **P1** | tinybvh 加速光追 | 1024×768/64spp 导出 ≥5× | 1 周 |
| **P1** | MO 等值面 + 能级图 | 一个 cube 组能出"轨道 + 能级"插图 | 1 周 |
| **P2** | DOS/PDOS、CDD、自旋密度 | 各出一个可导出 PNG 的面板 | 各 3–5 天 |
| **P2** | 光追材质升级（GGX/AO/降噪） | 与 Tachyon 观感接近 | 1–2 周 |
| **P3** | Vulkan RT 实时预览 / GPU 密度核 | 有硬件时帧率 ≥30 | 2–3 周 |

---

## 6. 风险与约束

1. **桌面锁屏问题**：GUI 类验收（LOD 观感、光追预览）需要解锁桌面，否则只能走 CLI 离屏；
2. **文件策略**：`src/`、`build/` 在 workspace-write 下不可写，需单次提权（每条命令一次批准）；
3. **依赖纪律**：引入 OSPRay/Embree 会增加打包体积与许可审查，优先 header-only（tinybvh）；
4. **回归基线**：每次改动后跑 `build\tests\Debug\moleculens_tests.exe`（当前 176 例 / 193,708 断言全过）；
5. **文档同步**：新增图类型必须同步 `docs/05` 的支持度对照表，并重出 DOCX。

---

## 7. 实施记录

### 2026-10-21 17:20 · P0 视锥剔除（已落地）

**改动**
- `draw_geometry.h`：`drawGeometry(..., const RenderView* view = nullptr)`（前向声明 `RenderView`）。
- `draw_geometry.cpp`：由 `view->camera.viewProjMatrix(aspect)` 提取 6 个视锥平面（Gribb–Hartmann），对
  - 球体实例：包围球 `(posRadius.xyz, posRadius.w)`；
  - 键圆柱：线段中点 + 半长 + 半径 的包围球；
  做剔除后再进 `drawModeRuns`。未传 view 时完全走原路径（零拷贝、零开销）。
- `vulkan_renderer.cpp` / `offscreen_renderer.cpp`：调用处传入 `&view`，实时视口与离屏导出都生效。
- CLI 新增 `--zoom <倍数>`（相机拉近），用于验证剔除。

**验证**
1. **正确性**：全视角（`md.gro` 2042 分子、1920×1080）渲染与改动前**逐字节相同**（md5 `87529a806e` 一致，0 个像素不同）→ 剔除没有误伤可见几何；
2. **回归**：全量 176 例 / 193,708 断言全过；
3. **放大 6× 目视**（`docs/figs_v2/cull_z6.png`）：画面完整、无缺块。

**实测（1600×1200，渲染总耗时）**

| 相机 | 耗时 |
|---|---|
| zoom = 1（全场景可见） | 1074 ms |
| zoom = 6 | 1622 ms |
| zoom = 12 | 1565 ms |

放大后**更慢**而不是更快——因为放大后球体铺满屏幕，**片元填充**成为瓶颈，剔除省下的顶点提交被淹没。这与 I-08 的结论一致：当前场景的瓶颈在填充率/同步，不在实例数量。

**结论**：剔除是正确的可扩展性优化（为 10⁵ 原子级体系、以及将来多视口/平移浏览兜底），但在 6.4k 原子场景上**看不出收益**。要真正提速，下一步应做：
1. 密度核 SIMD——影响 surface/quicksurf 生成；
2. 查 present mode / 帧同步（I-08 的 64 FPS 天花板）；
3. 若做超大体系，再回来量剔除的实际 draw 数（需要加计数打点）。

### 2026-10-21 18:15 · P0 QuickSurf 提速（已落地，输出逐位一致）

**改动**
1. `src/engine/style/quicksurf/quicksurf_avx2.cpp` 重写为真正可并行的 AVX2 密度核：按 z 层切块，多线程写共享网格（每体素仍按原子序号累加 → 与串行逐位相同）；顺带修掉旧版内层循环把 8 个体素写成同一指针的 bug。
2. `src/engine/style/quicksurf/quicksurf.cpp`：
   - 分派前按 z 层切分原子，密度计算多线程化（`std::thread` + 原子计数器）；
   - Marching Cubes（`extractSurface`）按 z 层并行，每层局部缓冲，最后按 z 顺序拼接 + 索引整体平移 → 三角形顺序与串行版完全一致；
   - 小网格（<64K 体素）不启线程，避免 2042 个小分子场景每次新建 16 个线程；
   - 新增环境变量 `MOLECULENS_QUICKSURF_BACKEND=auto|scalar|avx2|cuda` 与 `MOLECULENS_QUICKSURF_THREADS=N`（基准测试 / 低功耗）。
3. 构建：`quicksurf_avx2.cpp` 单文件加 `/arch:AVX2`（此前文件名叫 avx2，但工程没开该开关，实际一直走标量）。

**实测（md.gro，6452 原子，网格 100×110×101，sp=0.5，Release）**

| 配置 | density | MC | 合计 |
|---|---|---|---|
| 改前（AVX2 未生效 + 单线程） | 103.4 ms | 62.6 ms | 166 ms |
| 改后 threads=1 | 103.4 ms | 62.6 ms | 166 ms |
| 改后 threads=4 | 26.3 ms | 34.6 ms | 61 ms |
| 改后 threads=8 | 17.8 ms | 31.1 ms | **49 ms（3.4×）** |

Debug 构建同样加速（密度核 582→66 ms；MC 450→198 ms @4 线程）。

**一致性验证**：threads=1/4/16 输出 PNG md5 完全相同（`b676bb19…`）；逐实例 QuickSurf 输出与改前基线 `eadb3d2b…` 完全相同；176 例 / 193,708 断言全过。

**混合核提示**：i7-13650HX 为 6P+8E。实测 8 线程已接近最优；Debug 下 16 线程 MC 反而变慢（Debug 堆锁 + E 核调度），Release 无此问题。

### 2026-10-21 18:25 · 逐实例 QuickSurf 缓存：尝试后**回退**（记录取舍）

**动机**：GUI 里对溶剂盒选 QuickSurf 时走的是"每个分子一个等值面"（2042 个水分子 → 846 ms / 316 MB 命令流），是用户能直接感知的卡顿来源。

**做法**：按「结构索引 + 半径缩放 + 网格间距 + 等值面 + 掩码模式」做键，缓存以首个原子为原点的**局部**网格，其余实例只做平移复用。

**结果**：缓存命中率达标（2042 个实例只建 7 次网格），Debug 下 11867 ms → 5650 ms（**2.1×**）。

**为什么回退**：局部网格的 Marching Cubes 栅格相位与原先的"世界坐标栅格"不同，同一个水面偏移最多半个栅格（0.25 Å），与改前基线的像素差达 **21.9%**。虽然几何上等价、且同种分子彼此更一致，但这属于"没有逐位验证的视觉改动"，不应在收尾阶段合入。

**结论**：回退后逐实例输出与基线 md5 完全一致（`eadb3d2b…`）。若将来要这笔 2×，需要先决定：接受表面相位变化（并同步重出插图），或改成"世界栅格 + 按实例相位分桶缓存"。已记入 `ISSUES` I-17。


### 2026-10-21 19:00 · P1 CPU 光追 BVH 加速（已落地，输出逐位一致）

**改动**
- `src/engine/raytrace/ray_tracer.cpp`：新增 `BvhPrim` / `Bvh`（质心中位数二分、叶 4 图元、AABB slab 测试），`trace()` / `occluded()` 改为 BVH 遍历，线性版本保留为 `traceLinear()` 回退；`rayTrace()` 入口按场景建一次 BVH（追踪期间只读 → 多线程安全）。
- 环境变量 `MOLECULENS_RT_NO_BVH=1` 可强制线性路径（A/B 对比与回归校验用）。
- `src/cli/cmd/render/render_cmd.cpp`：新增 `--rt N`（N>0 时走 CPU 光追出图，省略 Vulkan），日志行 `[cli] rt: ... 球 + ... 胶囊, N spp, WxH, X ms`。
- `tests/engine/raytrace/ray_tracer_tests.cpp`：新增「BVH 与线性遍历逐像素一致」用例（32 球 + 12 胶囊，96×72）。

**实测（Debug，md.gro = 6452 球 + 4436 胶囊）**

| 分辨率 / 采样 | BVH | 线性 | 加速比 |
|---|---|---|---|
| 480×360，1 spp | **138 ms** | 31,904 ms | **231×** |
| 960×720，4 spp | **1,631 ms** | ≈ 8.5 min（外推 16×） | ≈ 310× |

**一致性**：480×360 1 spp 两条路径输出 PNG **md5 完全相同**（`D7EE0038…`）；177 例 / 193,713 断言全过。

**说明**：BVH 只改变"测哪些图元"，命中判定仍是解析求交，因此与线性版逐位一致；P1 首版只做了球/胶囊层，三角形网格随后在 19:15 补上（见下条），自研 BVH 已够用，tinybvh 可继续观察。

---

### 2026-10-21 19:15 · P1 补完：三角形网格光追（surface / QuickSurf 也能 RT）

**改动**
- `src/engine/raytrace/ray_tracer.cpp`：`BvhPrim::kind = 2` 纳入三角形；`trace()` / `occluded()` 增加 Möller–Trumbore 求交（双面，法线按重心插值后翻转朝外，`RtTriangle` 的 n0/n1/n2 + color/ambient/specular/shininess 参与着色）；`Bvh::build` 把 `scene.triangles` 一并加入；`buildRtScene()` 新增 `Cmd::TriMesh` 分支——按 `TriMeshHeader`（vertexCount/indexCount）+ `TriMeshVertex` 解包，**顶点色与 `cur.diffuse` 相乘**（复刻 GPU 侧 `bakeMaterialIntoColor`），三角形颜色/材质取三顶点平均。
- `src/cli/cmd/render/render_cmd.cpp`：`[cli] rt:` 日志增加三角形数量。
- `tests/engine/raytrace/ray_tracer_tests.cpp`：BVH 等价用例加入一个立方体（12 三角形），与线性路径仍逐位一致 → **177 例 / 193,713 断言全过**。

**实测（Debug，md.gro → surface 表示生成 911,096 个三角形）**

| 分辨率 | 耗时（含建 BVH） | 说明 |
|---|---|---|
| 32×24 | 5,282 ms | 几乎全是建树开销（911k 三角形，Debug） |
| 960×540 1 spp | 6,467 ms | 建树 ~5 s + 追踪 ~1.2 s（≈2.3 µs/射线） |

线性路径在 911k 三角形下不可行（240×180 跑 >10 分钟未完成，已终止），正确性由单元测试（立方体 12 三角形逐位一致）保证。
产物：`.monitor\rt_surf_960.png`（光追，球面平滑，逐分子着色正确）、`.monitor\raster_surf_960.png`（光栅版对比图）。

**遗留（P2 候选）**
1. **网格规模浪费**：911k 三角形 / 2042 水分子 ≈ 446 三角形/分子——surface 表示给每个分子单独细化了完整球面。可改为「共享单位球网格 + 逐实例变换」，三角形数可降 1–2 个数量级，建树从 5 s 降到几十 ms。
2. **RT 里表面是不透明的**：GPU 路径 surface 默认半透明（opacity<1），CPU 光追目前忽略 opacity，视觉上更"实"。要么在着色时按 opacity 混合背景，要么 RT 只用于不透明表示。
3. 建树目前单线程 + Debug 版极慢，Release 会快数倍；若要交互级 RT 需把建树做多线程或增量（BVH 已按质心中位数二分，改并行不难）。
---

### 2026-10-21 20:15 · P1 后续：并行建树 + 一次前提纠正

**前提纠正（重要）**：先前判断「911k 三角形 = 每个分子单独细分的球面，可换成共享球网格」是**错的**。surface 表示走的是 `src/engine/style/surface/surface.cpp:287 drawSystemSurface()`——把当前体系全部原子聚合成一个整体 Marching Cubes vdW 表面，911,096 个三角形是真实网格（约 141 三角形/原子）。因此不存在「共享单位球 + 实例变换」可复用，要减三角形只能降网格精度（改了几何）或做网格简化（decimate），两者都会改变渲染结果。

**真正做了的事**
1. **`Bvh::build` 并行化**（`src/engine/raytrace/ray_tracer.cpp`）：`nodes` 一次性预留满二叉上界（`prims*2+1`），用 `std::atomic<int32_t>` 游标领下标；`buildRange` 在区间 ≥8192 图元且深度 <6 时把左子交给 `std::thread`、右子本线程算，两者只写不同节点、切分 `prims` 不相交区间，无需加锁。线程预算 `min(8, 硬件线程/2)`，`MOLECULENS_RT_THREADS=N` 可覆盖。
2. **修掉一处我先前引入的语法脏点**：第 13 行 `#include <cstdlib>` 后混进了字面量 `` `n ``，导致 `#include <cstring>` 实际没生效、MSVC 报 C4067；已拆成两行。
3. 追踪本身就是按行分带的 `std::thread`（`ray_tracer.cpp` 末尾），这块不需要再改。

**实测（Release，md.gro → surface 911,096 三角形）**

| 场景 | 结果 |
|---|---|
| 建 BVH（32×24，几乎纯建树） | 单线程 172 ms → 并行 **118 ms**（1.46×） |
| 960×540 1 spp | **233 ms** |
| 1920×1080 1 spp | **511 ms** |
| Debug 同参数（对照） | 5,282 / 6,467 ms —— 之前的「5 s 建树」是 Debug 数字，Release 只有 0.1 s 量级 |

**一致性**：并行/单线程输出 PNG md5 均为 `a7e4d3b73c2d11824394323f70de4c74`；177 例 / 193,713 断言全过。

**叶子大小试验**：`kLeafSize` 4 → 16 时 960×540 从 203 ms 变 216 ms（更慢），已回退到 4。

**结论**：CPU 光追在建树上已不是瓶颈（118 ms 中含约 70 ms 的场景解包），继续压三角形数收益有限。要让光追「好用」，优先级应该是：① surface 半透明（当前忽略 opacity）+ 环境光遮蔽；② GUI 里跨调用的 BVH 缓存（同一场景改视角重复导出可省掉建树）；③ 再考虑网格简化。

---

### 2026-10-21 20:20 · 三角形光追性能收尾（含一次自造回归的修复）

**新事实**：surface 表示不是「一个整体网格一次发射」，`appendTriMesh` 会按单命令 16 MiB 上限切块——md.gro 的 911,096 三角形实际是**上千个 TriMesh 命令块**拼起来的。

**我引入又修掉的回归**：为了并行解包，我给每个 TriMesh 块加了 `out.triangles.reserve(size + triCount)`。因为块数上千，这等于在一个 100 MB 级 vector 上反复整体搬移，`buildRtScene` 从 0.26 s 劣化到 **27 s**（RT 出图进程总时长 2.9 s → 30.1 s）。修法：循环前先预扫一遍命令头（只读 `TriMeshHeader`）统计三角形总数、一次性 `reserve`；解包仍按 32k 三角形分块并行，但只对真正的大块（QuickSurf 整体系）生效。

**实测（Release，md.gro，960×540，1 spp）**

| 表面网格 `--surf-grid` | 三角形数 | 场景解包 | RT 总耗时 |
|---|---|---|---|
| 0.5（默认） | 911,096 | 26 ms | 213 ms |
| 1.0 | 203,944 | 6 ms | 88 ms |
| 1.5 | 88,012 | 2 ms | 77 ms |
| 2.0 | 34,068 | 1 ms | 62 ms |

- 纯建树（32×24 出图，几乎只有建树）：911k 三角形 **119 ms**；34k 三角形 **5 ms**。
- QuickSurf 整体系（2,028,672 三角形）：解包 57 ms，960×540 总 422 ms。
- 建树并行调参（阈值 8192→2048、深度 6→10、预算 8→16）实测反而更慢（132 ms），已回退；瓶颈是内存带宽不是线程数。
- 一致性：输出 md5 全程保持 `a7e4d3b73c2d11824394323f70de4c74`；177 例 / 193,713 断言全过。

**结论**：「911k → 几万三角形、建树几十毫秒」只能靠降低表面网格精度实现（`--surf-grid 2.0` 即 34k 三角形 / 建树 5 ms / 全流程 62 ms），代价是 vdW 表面变钝、细节丢失。若希望 RT 预览默认快，建议把「预览用粗网格、导出用细网格」做成 RT 的 LOD 策略，而不是改进 BVH。

---

### 2026-10-21 21:00 · RT 级 LOD（交互降档 + 场景缓冲复用）

按 20:20 的结论落地「预览用粗、导出用细」的 RT LOD。实现分三层，都在 GUI 预览链路内：

1. **场景缓冲复用**（最大收益）：`src/engine/raytrace/ray_tracer.{h,cpp}` 新增 `struct RtAccel`（内含 BVH）与 `buildRtAccel(scene)`；`rayTrace(...)` 多一个可选参数 `const std::shared_ptr<RtAccel>& accel`，传入时跳过建树。`src/app/view/viewport/mol_scene.{h,cpp}` 在 `MolScene` 里缓存 `rtSceneCache_` + `rtAccelCache_`，键为 `renderer_->commandSerial()`：相机移动不再重建 911k 三角形的 RtScene 与 BVH。
2. **分辨率两档**：相机在动 → 追踪宽度 320、1 spp；停手 ≥ 0.25 s → 自动升到 720、2 spp 重渲一次，下一次交互自动退回低档。`rtRefined_` 标记当前纹理档位。
3. **纹理尺寸恒定**：纹理固定按高档尺寸创建，低档结果在 CPU 侧最近邻放大后上传。**这一步是修 bug 的关键**——此前随档位改纹理尺寸，每次切换都会 `destroyRtTexture()` + 重建 `VkImage`/描述符，而上一帧的 ImGui 绘制仍引用旧描述符，实测刷屏 `vkQueueSubmit failed`（旧实例 20:56:54–20:56:57 共 331 条）。

**实测（Debug，md.gro 6452 原子，surface 表示 911,096 三角形，视口 848×582）**

| 场景 | 分辨率 | spp | 耗时 | 说明 |
|---|---|---|---|---|
| 首次开启（命令流变化） | 320×219 | 1 | **2311 ms** | 解包 + 建树一次性成本 |
| 交互中（相机在动） | 320×219 | 1 | **39–61 ms** | 缓存命中，只剩投射 |
| 停手 0.25 s 后升级 | 720×494 | 2 | **360–526 ms** | 自动高清档 |

**Release 对照（CLI，同一场景，32×24 出图隔离建树）**：全流程 103 ms（解包 25 ms + 建树 ~75 ms）。即 Release 下首次开启约 0.1 s、交互帧约 5–8 ms、高清档约 50–70 ms。

**验证**：177 例 / 193,713 断言全过；新实例（20:57:25 起）RT 预览全程 `[error]` 计数为 **0**；拖动/停手两档切换正常出图。

**后续可做**：把两个档位的分辨率与 spp 做成设置项（`Settings::rtPreviewSpp` / `rtPreviewCap`）；低档时按屏幕空间误差选更低 `surfGridSpacing` 的 LRU 网格缓存（20:25 已回退的逐实例缓存是同一思路，但需接受相位差）。

---

### 2026-10-21 21:30 · v2.6 玻璃观感落地（Glass 对标）

用户给了 4 张对照图（自建引擎的白底半透明聚合物 vs 参考实现的 QuickSurf + DynamicBonds 玻璃壳），要求「实现图片相同风格的渲染作为检验」＋「UI 优化」。

**渲染侧**
- `shaders/mesh.frag`：半透明片元加视相关玻璃壳——边缘抬 alpha（`fres = pow(1-N·V,3)`）、边缘吸收压暗（`×0.72`）、`rim = pow(1-N·V,6)` 叠冷调反射与材质掠射高光。不透明片元零影响。
- 材质仍用 `kMaterialPresets` 里的 Glass1/2/3（参数按经典材质库校准），本次只补"视相关"这一层，不改参数表。

**入口侧（此前缺失）**
- CLI：`--material <预设名>`；`--rep` 规格第 5 段可写材质；`--system-surface` 修正为对 `--rep` 也生效（否则多表示 + 整体系表面会退化成逐分子壳）。
- GUI：显示模块顶栏「一键风格」→ **玻璃**（整体系 Glass2 表面 0.32 + 球棍 Opaque + 白底 + Name 配色）/ **不透明**。

**对照结果**：`.monitor/polymer.gro`（md.gro 去 SOL，356 原子）在 1100×950 / SSAA 2× 下表面 26,660 三角形，玻璃壳有明暗层次、掠射高光与壳内球棍透视，见 `docs/figs_v2/glass_D.png`。

**遗留**：① 真正对标还需要同机同结构参考截图逐项比对；② 用户要的「UI 优化」目前只落到一键风格，面板分层/密度尚未动；③ 玻璃表面在 RT 下未处理折射/反射（P2 tinybvh 之后再谈）。

## 2026-10-21 23:30 · v2.9 分子标注 + 多选 + 自定义内容区

- **视口内分子名标注**：走命令条「分子名」开关（成员 `Workbench::showMolLabels_`，`src/app/workbench/workbench.h`）。实现位置 `src/app/workbench/workbench.cpp` 中 `svc_.viewport->render(canvas)` 之后：
  - 每帧从 `svc_.scene->instances()` 取每个结构的第一条实例，用 `globalAtomBase(li)` 定位 `coordsSoA().positions` 中的坐标段（段长 = `structuresPool_[sidx].elements.size()`），求质心，再经 `engine::esp_overlay::projectToPixel(cam->viewProjMatrix(canvas.x/canvas.y), ...)` 投影成像素。
  - 标签画在 `ImGui::GetWindowDrawList()` 上：深色半透明圆角底 + 淡蓝描边 + 白字，避免白底/深底看不清。
  - **防重叠**：维护 `std::vector<ImVec4> lbPlaced`，与已放置矩形相交就向下错开 16px（最多 6 次）。
- **表示表格重排**（`src/app/workbench/modules/modules.cpp` 的 `anchorRep`/`tableRep`：表示表格列：可见 ✓ | 名称 | 样式 | 配色 | 选择 | ⋯ 菜单；用 `ImGui::TableSetupColumn` 固定列宽，全部 `PushID(row)` 保证控件唯一。东/西侧用 `ImGui::TableSetupScrollFreeze(0,1)` 固定表头。
- **多选 + 批量编辑**（`Workbench` 新增 `std::vector<Representation> selList_`）：Ctrl/Shift 点行切换选择（`repRowCtl` 与多选条都改 `selList_`），多选后出现的批量条支持「批量样式 / 批量配色 / 批量可见 / 删除选中」。
- **删除快捷键**：表示表格获得焦点时 `Del` 删除选中表示。
- **自定义内容区**（自绘双栏布局）：`renderViewport+Inspector` 内若 `aspect < 0.9`，用 `ImGui::BeginChild("vp_col")` / `BeginChild("insp_col")` 两列排列（左视口、右检查器），`splitter` 拖拽值 0.92–0.982 存 `inspFrac_`（默认 0.965）并持久化到 `Settings::inspectorFraction`。
- **进展**：Debug 构建 exit 0；177 例 / 193,713 断言全过；GUI 实机确认标签与表格。
### 23:35 · 表示表格单元格可编辑（表格式）
- `renderRepTable`（`src/app/workbench/modules/modules.cpp`）的「样式」「配色」两格从 `TextDisabled` 改为 `Selectable` + 弹出选择列表：样式列用现有 `kTypes`（13 项，`TypeEntry{RepresentationType, const char*}`），配色列遍历 `engine::ColorScheme::Count` 用 `engine::colorSchemeZh(cs)` 取名。窄面板（`availW < 430`）合并为一格，弹出菜单分「绘制样式 / 配色」两段。
- 行尾原「×」删除按钮紧贴选择列，实测容易误删 → 改为「...」小按钮 + 弹出菜单（「删除此表示」「设为编辑目标」）。注意 ImGui 默认字体没有 `⋯`(U+22EF) 字形，会显示成 `?`，用 ASCII 三点。
- 实机验证：样式列点「球棍」→ 选「范德华球」视图立刻变空间填充；配色列弹出 6 个方案；悬停提示「点击修改配色方案」正常。
### 23:50 · 表示表格多选 + 两个崩溃/断言修复
- 多选：`Workbench` 新增 `repMultiSel_` 与 `isRepMultiSelected / toggleRepMultiSelected / clearRepMultiSelection / repMultiSelection()`（`workbench.h`、`workbench.cpp`）；表格名称列支持 Ctrl/Shift 点选，多选 ≥2 时表下出现批量条（批量样式 / 批量配色 / 批量可见 / 批量删除，后者按行号倒序删）。
- 修 **I-19**（新建表示即崩溃 0xC0000005，引用悬垂 → 改为按值快照）、**I-20**（表格内 PushID 被列 ID 覆盖导致 ID 冲突断言 → 控件自带 `##行号` 后缀，行选中改用 `TableSetBgColor`）。
- 构建 exit 0；177 例 / 193,713 断言全过；实机确认多选与批量条。
### 23:55 · 实机验证记录（v2.9 表示管理收尾）
- 多选：Ctrl 点两行 → 表下方「已选 2 个表示」+ 批量条；「批量样式 → 线框」两行同时生效、视口即时变线框；再切「球棍」两行同时恢复。
- 单点单元格：点样式列「球棍」→ 选「范德华球」，该行与下方表单同步，视口变空间填充。
- 期间修掉两个阻断问题：I-19（新建表示引用悬垂崩溃）、I-20（表格内行级 PushID 被列 ID 覆盖导致的 ID 冲突断言）。
- 最终状态：`build exit=0`、177 例 / 193,713 断言全过、GUI 无崩溃无断言。
### 2026-10-22 00:39 · RT 级 LOD 实机复核（小场景）
- 场景：`other/qsurf.mlscene`（35 原子，QuickSurf 2952 三角形），视口 648×582。
- 日志实测：首帧场景重建 `320×287 1spp 88 ms`；交互期 `320×287 1spp 80–98 ms`（场景缓存命中，不重建 RtScene/BVH）；停手 0.25 s 后 `648×582 2spp 641–692 ms`。
- 整个会话 `[error]` 计数 0，说明纹理固定尺寸 + CPU 放大的修法稳住了（对比 I-18 那次的 331 条 vkQueueSubmit 报错）。
- 结论：交互档比静止档快 6–8 倍，且静止图是全分辨率 2spp；切档不再触发纹理重建。
