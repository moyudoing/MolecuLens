# MolecuLens v2 文档 05 · DFT 常用绘图指南

| 项目 | 内容 |
| --- | --- |
| 文档编号 | 05 |
| 文档名称 | DFT 常用绘图指南（工具与方法） |
| 版本 | 1.0 |
| 日期 | 2026-10-21 |
| 适用产品 | MolecuLens v2.2.0（Aurora） |
| 读者 | 用 DFT 出图的科研用户、项目评审、二次开发者 |
| 上游依据 | `ESP_SURFACE.md`、`docs/01`–`docs/04`、`src/`（2026-10-21 代码事实） |
| 定位 | **讲「图有哪几类、数据从哪来、用什么工具画」**，不是功能宣传；MolecuLens 做不到的如实标 ✗ 并给替代工具 |

> **阅读约定**：**✅** = MolecuLens 原生支持；**◐** = 部分支持（需要外部数据、间接路径或只能显示不能计算）；**✗** = 不支持（给出替代工具）。所有「MolecuLens 能做」的说法都能在 `src/` 找到对应实现；反过来，标 ✗ 的能力**当前没有任何代码**，不要按已完成排期。
>
> 本文不重复《02 功能介绍书》的能力矩阵，也不重复《03 使用说明》的分步操作；参数细节以 `ESP_SURFACE.md` 与 03 §4.2 为准。

## 目录

- 1 先分清四类问题
- 2 单位、符号与坐标（必读）
- 3 静电势映射表面（文献常称「能量势」）
- 4 分子轨道等值面与能级图
- 5 DOS / PDOS
- 6 电荷密度差 CDD 与形变密度
- 7 自旋密度
- 8 NCI / RDG / IGMH
- 9 势能面扫描、能垒与 IRC
- 10 能带结构（周期体系）
- 11 其它常用图
- 12 工具矩阵与分工
- 13 出图规范
- 14 MolecuLens 支持度对照与路线图
- 15 十个最常见的坑
- 16 与其它文档的关系
---

## 1 先分清四类问题

同样是「DFT 出图」，要回答的科学问题不同，数据来源和画法完全不同。先定位问题，再选工具，能省掉一半返工。

| 问题类型 | 典型图 | 数据从哪来 | 常用工具 |
| --- | --- | --- | --- |
| **电子结构在空间上长什么样** | ESP 映射表面、分子轨道、CDD、自旋密度、ELF/LOL、NCI | 波函数/密度 → 三维网格（cube / CHGCAR） | Multiwfn、VASPKIT、NCIPLOT、critic2 → MolecuLens / VESTA |
| **能量上怎么分布** | 能级图（HOMO/LUMO）、DOS/PDOS、能带 | 轨道能量、本征值、投影态密度 | Multiwfn、VASPKIT、pymatgen、LOBSTER → Origin / gnuplot / matplotlib |
| **反应怎么走、能垒多少** | 势能面扫描、IRC、能垒图 | 一系列单点能 / 优化结构 | Gaussian、ORCA、ASE → Origin / gnuplot / matplotlib |
| **定量与切片** | 静电势切片/等值线、原子电荷着色、表面积极值统计 | 网格数据的二维切片或逐原子标量 | Multiwfn、VESTA、gnuplot、matplotlib |

一条实用原则：**「算」和「画」尽量分开**。量化软件负责产生数据（cube、CHGCAR、能量表），后处理工具负责把数据变成等值面或曲线，绘图工具只负责版面。MolecuLens 站在「画」这一层——它不内建任何量化计算，也不解析 `.fchk`。

---

## 2 单位、符号与坐标（必读）

### 2.1 换算表

| 物理量 | 从 | 到 | 乘数 | 说明 |
| --- | --- | --- | ---: | --- |
| 长度 | Bohr | Å | **0.529177** | cube 文件坐标默认 Bohr；MolecuLens 读入后统一 Å |
| 能量 | Hartree | kcal/mol | **627.509474** | ESP 数值换算；本文所有 ±45 等默认值都是 kcal/mol |
| 能量 | Hartree | eV | **27.2114** | 轨道能、能带、DOS 横轴常用 |
| 能量 | eV | kcal/mol | 23.0605 | 跨文献比较时用 |
| 能量 | Hartree | kJ/mol | 2625.50 | 部分热化学文献用 |
| 长度 | Å | nm | 0.1 | 轨迹/盒子长度常见 |
| 密度 | e/Bohr³ | e/Å³ | **6.74833** | 1 Bohr³ = 0.148185 Å³ |
| 静电势 | a.u.（Hartree/e） | kcal/mol | 627.509474 | 与能量同乘数 |

> **最常出错的一处**：cube 文件里的坐标是 **Bohr**，但大多数量化软件在日志里打印的键长是 Å。把日志里的几何直接和 cube 对比时，先统一单位，否则会得到「差 1.89 倍」的假象。

### 2.2 符号

| 符号 | 含义 | 常用单位 |
| --- | --- | --- |
| ρ | 电子密度 | a.u.（e/Bohr³） |
| V_ESP | 静电势（核 + 电子） | kcal/mol（MolecuLens 显示）、a.u.（cube 内） |
| ρ_spin | 自旋密度 ρ(α) − ρ(β) | a.u. |
| Δρ / CDD | 电荷密度差 | a.u. |
| sign(λ2)ρ | 电子密度在键临界点处 Hessian 第二特征值的符号 × ρ | a.u. |
| s / RDG | 约化密度梯度 | 无量纲 |
| ELF / LOL | 电子局域化函数 / 局域化轨道定位函数 | 0–1 无量纲 |
| ε_i | 第 i 个轨道能 | eV 或 Hartree |
| E_F | 费米能级（周期体系） | eV |

### 2.3 cube 文件的三个约定

1. **坐标单位是 Bohr**，原点、三个轴向量、体素步长都是 Bohr；
2. **数值单位由产生它的程序决定**：电子密度/轨道/自旋密度一般是 a.u.，ESP 是 Hartree/e，CDD 也是 a.u.——**同一个 `.cube` 扩展名可以装不同物理量**；
3. MolecuLens 读 cube 时把坐标转成 Å，ESP 数值按需转 kcal/mol；**等值面阈值仍按 cube 原始单位解释**（所以密度用 0.001、轨道用 0.02，数值差三个数量级是正常的，不是 bug）。

---

## 3 静电势映射表面（文献常称「能量势」）

「能量势」在文献里通常指 **ESP（electrostatic potential）映射到电子密度等值面上**：表面形状来自 ρ，颜色来自 V_ESP。它回答的是「分子表面哪里富电子、哪里缺电子」，不是总能量。

| 项 | 内容 |
| --- | --- |
| 适用问题 | 亲电/亲核位点、σ-hole、氢键给受体、分子间识别、溶剂化趋势 |
| 数据来源与格式 | **两个 cube，必须同一次计算、同一网格**：`density.cub`（几何）+ `totesp.cub`（颜色）。由 `.fchk` 经 Multiwfn 导出 |
| 方法与关键参数 | 密度等值面 iso = **0.001 a.u.**（≈ vdW 面；GUI 滑杆范围 **0.0005–0.02 a.u.**，别与 Surface/QuickSurf 的 0.05–2.0 混淆）；色标默认 **±45 kcal/mol**；自动范围取 **±99.5 分位**；Hartree→kcal/mol **×627.509474**；表面不透明度建议 **0.6–0.85** |
| 工具 | Multiwfn 生成 cube；MolecuLens 渲染（GUI「ESP」模块 / CLI `--esp`）；替代：GaussView + 自写脚本 |
| MolecuLens 现状 | **✅ 原生**：密度等值面 + ESP 逐顶点着色 + 色标条 + 极值标注，导出 PNG 时一并写入 |
| 坑 | 两个 cube 网格不一致 → 表面出现或全灰；iso 过大表面缩进分子内部；跨分子比较时忘了固定 vmin/vmax（自动范围各自不同，颜色不可比）；把 ESP 当成「总能」解释 |

**Multiwfn 导出（与 `ESP_SURFACE.md` 一致）**：

1. Gaussian 完成优化/单点，得到 `.fchk`；
2. Multiwfn 载入 fchk，走 `5`（输出 cube）→ `1`（电子密度）→ `3`（网格设置）→ `2`（输出 `density.cub`）；
3. 再走 `5` → `12`（静电势）→ 输出 `totesp.cub`（总静电势：核 + 电子，决定颜色）；
4. 两个 cube 的网格维度、原点、步长必须一致——Multiwfn 默认沿用同一网格，**不要**手工改其中一个。

**MolecuLens 复现命令（CLI，Release）**：

```powershell
.\moleculens-cli.exe render density.cub `
  --esp totesp.cub `
  --iso 0.001 `
  --cmap rwb `
  --esp-opacity 0.85 `
  --size 1600 --height 1200 --scale 3 --bg white `
  --out esp.png
```

**参数要点**（完整表见 `ESP_SURFACE.md` 与 03 §4.2）：

| 参数 | 默认 | 建议 |
| --- | --- | --- |
| `--iso` | 0.001 a.u. | vdW 面用 0.001；更外扩 0.0005；更贴身 0.002–0.005 |
| `--vmin` / `--vmax` | 自动（±99.5 分位） | **同一组图必须固定同一范围**，例如统一 −45/+45，否则颜色不可比 |
| `--cmap` | `reference` | 化学惯例用 `rwb`（红=负/富电子，蓝=正/缺电子）；色盲友好用 `coolwarm` / `viridis` |
| `--esp-opacity` | 0.85 | 要看清内部球棍用 0.6–0.75；要颜色饱和用 0.95–1.0 |
| `--invert` | 关 | 只在色标方向与文献相反时用，且在图注里写明 |

**已知边界**（与 `ESP_SURFACE.md` 一致）：极值点是引擎自动检测（顶点邻域 + **1.2 Å 非极大抑制**，最多 **8 极大 + 8 极小**），与 Multiwfn `surfanalysis.pdb` 可能有位置/数量差异，投稿前人工核对；半透明表面只绘正面（背面剔除），不做 OIT；ESP 表面不参与阴影投射。

---

## 4 分子轨道等值面与能级图

| 项 | 内容 |
| --- | --- |
| 适用问题 | HOMO/LUMO 形状、成键/反键性质、孤对与空轨道、激发定域性 |
| 数据来源与格式 | **每个轨道一个 cube**（Gaussian `cubegen` 或 Multiwfn 输出；ORCA 用 `orca_plot`）。cube 数值 = 该轨道的波函数幅值（有正负） |
| 方法与关键参数 | 单 cube 等值面；典型 iso **0.02–0.05 a.u.**；同一组图必须用**同一 iso**（或用工具提供的「包络百分比」模式，但不同软件定义不同）；正负号用双色区分 |
| 工具 | 轨道 cube：Gaussian / Multiwfn / ORCA；等值面：MolecuLens（轨道模块）、GaussView；能级图：GaussView / Chemcraft / Origin / gnuplot / matplotlib |
| MolecuLens 现状 | **◐**：单个 cube 的等值面 ✅（「轨道」模块，iso 0.0005–0.5，默认 0.02），颜色即数值大小，**能显示正负两色**；但**不解析 fchk/MO、不能按轨道号选择、没有能级图/节面/双等值面**（路线图） |
| 坑 | 不同软件的 cube 归一化不同，绝对 iso 不可跨软件比较；iso 太小会出现碎片；把 HOMO/LUMO 画在同一 iso 下比较大小是常见做法，但**形状差异不等于能量差** |

**做法（MolecuLens 侧）**：

1. 先用外部工具把一个轨道导出为 `.cube`（文件名自带轨道号，便于追溯）；
2. MolecuLens 打开该 cube → 左导航「轨道」→ 拖动「等值面阈值」（0.02 起）→「生成等值面表示」；
3. 同一 cube 同时作为几何与着色数据，正负值分别落在色标两端；
4. 需要并排比较 HOMO/LUMO 时，分别导出两个 cube，用**相同 iso 与相同色标范围**各出一张图，再在版面上拼接。

> 当前「轨道」模块的统计只有来源、网格、数值范围、原子数；**不含原点/步长/直方图**，这些请回原始 cube 或 Multiwfn 核对。

**能级图（MolecuLens ✗）**：从输出文件取轨道能（Gaussian 的 `Alpha occ./virt. eigenvalues`，ORCA 的 `ORBITAL ENERGIES`），以 HOMO 或真空能为零点画水平线；gap = LUMO − HOMO。TD-DFT 棒状谱需要激发能 + 振子强度，用 Origin/matplotlib 画。参考做法：

```text
# 伪代码：能级图
for each orbital i:  y = E_i (eV);  draw horizontal line from x0 to x0+width
label HOMO/LUMO; annotate gap = E(LUMO) - E(HOMO)
```

---

## 5 DOS / PDOS

| 项 | 内容 |
| --- | --- |
| 适用问题 | 电子结构总体分布、成键/反键区、催化与电池体系的 frontier 分析、杂化 |
| 数据来源与格式 | 分子：轨道能列表（Gaussian/ORCA 输出）→ 展宽成 DOS；周期体系：`DOSCAR`（VASP）或等价文件 |
| 方法与关键参数 | 高斯展宽 **0.2–0.3 eV**（FWHM）；能量零点统一到 **E_F**（周期）或 **HOMO**（分子）；PDOS 按原子/壳层/轨道投影；ΣPDOS ≈ TDOS（投影不完整时会差） |
| 工具 | 分子：Multiwfn（主功能 10「态密度」，菜单名以本机版本为准）；周期：VASPKIT / pymatgen / LOBSTER；画：gnuplot / Origin / matplotlib |
| MolecuLens 现状 | **✗**：无 DOS/PDOS 面板，也不解析 fchk；只能用外部工具出图 |
| 坑 | 展宽宽度直接改变峰形与「带隙表观宽度」；不同程序对 E_F 的定义不同；PDOS 投影方案（site vs orbital vs Löwdin/Mulliken）不可混用；分子体系谈「带隙」要说明是 HOMO–LUMO gap 而非固体带隙 |

**典型流程**：Gaussian/ORCA → Multiwfn 生成展宽后的 DOS 数据（或 VASP → VASPKIT 导出）→ gnuplot/Origin 画 `E(eV)` vs `DOS(states/eV)`，把 E_F 或 HOMO 对齐到 0，PDOS 用堆叠或分面呈现。

```gnuplot
# gnuplot 最小示例：TDOS + 两条 PDOS
set xlabel "E - E_F (eV)"; set ylabel "DOS (states/eV)"
plot "tdos.dat" u 1:2 w l lw 2 t "Total", \
     "pdos_C.dat" u 1:2 w l t "C p", \
     "pdos_O.dat" u 1:2 w l t "O p"
```

---

## 6 电荷密度差 CDD 与形变密度

| 项 | 内容 |
| --- | --- |
| 适用问题 | 成键/吸附的电荷转移方向与大小、配位键、界面相互作用、形变密度 |
| 数据来源与格式 | 三个单点：**复合物 AB、片段 A、片段 B**，几何完全相同、泛函/基组/网格一致；`Δρ = ρ(AB) − ρ(A) − ρ(B)` |
| 方法与关键参数 | 同一网格逐点相减；等值面 **±0.005 a.u.**（常用 0.002–0.01）；正（积累）与负（耗散）分别用两个等值面/两种颜色；CDD 对基组与 BSSE 敏感 |
| 工具 | Gaussian `cubegen` / Multiwfn 网格运算 / VASP `CHGCAR` 相减（`ICHARG=11` 非自洽）；画：MolecuLens（只显示结果 cube）、VESTA |
| MolecuLens 现状 | **◐**：能把**已经算好的** CDD cube 当单 cube 等值面显示（正负值落在色标两端）；**不能**做三份密度的相减 |
| 坑 | 三个计算的网格/盒子必须完全一致；片段单点要用复合物几何（否则混入弛豫能）；BSSE 未校正时电荷转移会被高估；只画一个 iso 会漏掉另一半符号 |

**做法**：分别导出 `AB.cube`、`A.cube`、`B.cube` → 用 Multiwfn 的网格运算（或自写脚本）相减 → 得到带正负的 `cdd.cube` → 在 MolecuLens 里用「轨道」模块显示（iso 0.005，色标 `bwr`/`coolwarm`），正负两端分别截图或直接利用连续色标。

---

## 7 自旋密度

| 项 | 内容 |
| --- | --- |
| 适用问题 | 自由基/开壳层体系的未成对电子分布、磁性耦合、自旋污染判断 |
| 数据来源与格式 | ρ(α) − ρ(β) 的 cube（Gaussian 自旋密度、ORCA、VASP `ISPIN=2` 的 `CHGCAR` 差值） |
| 方法与关键参数 | 等值面 **0.005 a.u.**（0.001–0.01 视体系）；单色（只画正值）或正负双色；总自旋积分应等于 2S（自旋污染时不等） |
| 工具 | Gaussian / ORCA / VASP 产生；MolecuLens / VESTA 显示 |
| MolecuLens 现状 | **◐**：显示单 cube；无自旋密度计算、无自旋布居分析 |
| 坑 | 自旋污染（`<S²>` 偏离 S(S+1)）会让密度「看起来对、积分不对」；UKS 与 ROKS 结果不可直接比；离域体系 iso 太小会把整个分子染红 |

---

## 8 NCI / RDG / IGMH

| 项 | 内容 |
| --- | --- |
| 适用问题 | 非共价相互作用：氢键、π–π、CH–π、卤键、vdW 接触；配体-蛋白相互作用 |
| 数据来源与格式 |  promolecular 密度与 RDG（s）；NCI 图 = `s` 对 `sign(λ2)ρ` 的散点 + 梯度等值面；IGMH 对片段对做 δg 分解 |
| 方法与关键参数 | RDG 等值面常用 **s = 0.5**；着色用 `sign(λ2)ρ`（蓝=强吸引/氢键，绿=vdW，红=排斥）；δg 散点用于确认峰位；IGMH 需要片段划分 |
| 工具 | Multiwfn **主功能 20（弱相互作用分析：RDG/NCI/IGM/IGMH）**、NCIPLOT、IGMH_Toolbox；画：MolecuLens 可显示 RDG 等值面但无散点图 |
| MolecuLens 现状 | **✗ 计算**（无 NCI/IGMH 实现）；**◐ 显示**：把已生成的 RDG 等值面 cube 当普通体数据等值面画，无法做 `sign(λ2)ρ` 的 BGR 映射与 δg 散点 |
| 坑 | 用 promolecular 密度（而非真实密度）时峰位会偏移；等值面 iso 与散点阈值必须一起调；分子内 vs 分子间要分开看；NCI 图的「绿斑」不等于有强相互作用 |

---

## 9 势能面扫描、能垒与 IRC

| 项 | 内容 |
| --- | --- |
| 适用问题 | 构象搜索、反应坐标、过渡态能垒、取代基效应、催化循环能量图 |
| 数据来源与格式 | 一系列单点能（扫描/IRC/优化输出），或 `opt=modredundant` 的扫描结果；TS 需 `freq` 确认唯一虚频 |
| 方法与关键参数 | 相对能量 ΔE（可选 ZPE 校正、ΔG）；同一 level of theory、同一组成；TS 用 IRC 双向确认连接的反应物/产物；能垒 = E(TS) − E(反应物) |
| 工具 | Gaussian、ORCA、ASE 做计算；GaussView / Chemcraft 画曲线；cclib 解析输出；Origin / gnuplot / matplotlib 出图 |
| MolecuLens 现状 | **✗ 能量曲线**（无能量表/绘图面板）；**◐ 几何动画**：把扫描或 IRC 的逐帧结构转成 DCD/XTC 后可在「轨迹」模块播放，直观检查路径是否合理 |
| 坑 | 不同步骤用了不同泛函/基组；忘做 ZPE/热校正；TS 有多个虚频；IRC 跑断；能量零点不统一；把电子能垒当自由能垒讲 |

**最小流程**：优化反应物/产物 → 找 TS（`opt=(calcfc,ts,noeigentest)` 或 ORCA 的 `OptTS`）→ `freq` 验证唯一虚频 → IRC 双向 → 取各驻点能量画曲线。曲线本身 MolecuLens 不做，用 Origin/matplotlib；需要看几何变化时把 IRC 帧转成轨迹再播放。

---

## 10 能带结构（周期体系）

| 项 | 内容 |
| --- | --- |
| 适用问题 | 晶体/2D 材料/表面的电子结构：带隙、直接/间接、有效质量、轨道贡献 |
| 数据来源与格式 | VASP 的 `EIGENVAL` / `OUTCAR` / `DOSCAR`；高对称 k 路径由 `KPOINTS`（line mode）或 Seek-path 生成 |
| 方法与关键参数 | 能量零点取 **E_F**；横轴为 k 路径（Γ–X–M–Γ…）；带隙 = CBM − VBM；自旋极化体系 α/β 分开画；SOC 需非共线计算 |
| 工具 | VASP + VASPKIT / pymatgen / LOBSTER（投影/成键）；画：gnuplot / matplotlib / Origin；结构图：VESTA |
| MolecuLens 现状 | **✗**：不读周期性输出、无 k 路径/能带面板 |
| 坑 | E_F 取法不一致；k 路径与文献不同导致带隙「看起来不同」；slab 体系要说明真空层与偶极校正；未做 k 点收敛；把分子 HOMO–LUMO gap 与固体带隙混为一谈 |

---

## 11 其它常用图

| 图类型 | 数据与做法 | 关键参数 | MolecuLens | 替代工具 |
| --- | --- | --- | --- | --- |
| **ELF / LOL** | 波函数 → ELF/LOL 网格；等值面或切片 | ELF 0.5–0.8；LOL 0.5–0.6 | **◐** 显示 cube 等值面 | Multiwfn、VASP + VESTA |
| **静电势切片/等值线** | 平面网格（2D）→ 等值线/热图 | 平面位置、色标范围、等值线间隔 | **✗**（只有 3D 表面） | Multiwfn 导出 2D、gnuplot、matplotlib |
| **原子电荷着色** | 逐原子标量（Mulliken/Hirshfeld/ADCH…）→ 按值着色 | 电荷方案必须与文献一致 | **✗** 仅支持元素/链/残基/BFactor/单色 | PyMOL、Multiwfn、VESTA |
| **表面极值统计** | 表面局部极大/极小 + 数值 | 平滑半径、NMS 阈值 | **◐** 自动标注 8 极大 + 8 极小（1.2 Å NMS），**无面积/统计导出** | Multiwfn `surfanalysis` |
| **表面积 / 体积** | 等值面网格积分 | iso、网格密度 | **✗** | Multiwfn、MSMS、VESTA |

---

## 12 工具矩阵与分工

### 12.1 分层矩阵

| 层 | 工具 | 强项 | 边界 / 注意 | 许可 |
| --- | --- | --- | --- | --- |
| 算 | **Gaussian** | 分子 DFT/TD-DFT、fchk、cubegen | 商业；大体系慢 | 商业 |
| 算 | **ORCA** | 分子 DFT/MP2/CC、`orca_plot` 出 cube | 学术免费需注册；输入语法自成一套 | 学术免费 |
| 算 | **VASP** | 周期体系 DFT、`CHGCAR`/`DOSCAR` | 商业；需要赝势与 k 点收敛 | 商业 |
| 算 | ASE | 脚本驱动、批量扫描/IRC | 本身不是电子结构引擎 | 开源 |
| 后处理 | **Multiwfn** | ESP/轨道/DOS/NCI/IGMH/电荷/拓扑，事实标准 | 菜单式交互，批量需脚本；版本间菜单位置会变 | 免费 |
| 后处理 | **VASPKIT** | VASP 后处理（能带/DOS/电荷） | 依赖 VASP 输出格式；菜单号以本机版本为准 | 学术免费 |
| 后处理 | pymatgen | Python 解析 VASP/能带/DOS | 需要写代码 | 开源 |
| 后处理 | LOBSTER | 晶体轨道哈密顿布居（COHP） | 需要投影基组 | 学术免费 |
| 后处理 | critic2 | 拓扑分析（AIM/QTAIM） | 需要波函数/密度文件 | 开源 |
| 后处理 | NCIPLOT | NCI/RDG 专用 | 主要面向 promolecular 密度 | 学术免费 |
| 后处理 | cclib | 统一解析 Gaussian/ORCA 等输出 | 只解析，不计算 | 开源 |
| 画 | **MolecuLens** | ESP 表面、体数据等值面、轨迹、导出 PNG（1×–4×/4K/透明） | 不内建量化计算、不解析 fchk | 本项目 |
| 画 | **VESTA** | 晶体结构、CHGCAR 等值面、能带/DOS 数据可视化 | 分子体系观感一般 | 免费 |
| 画 | GaussView / Chemcraft | 轨道、振动、能级图、建模 | 商业（Chemcraft 有试用） | 商业 |
| 画 | Avogadro | 建模 + 简易可视化 | 大体系与出版级出图有限 | 开源 |
| 画 | PyMOL | 蛋白/配体、卡通、表面 | 出版风格偏生物 | 开源版 + 商业版 |
| 画 | Origin / gnuplot / matplotlib | 曲线/散点/能级图/DOS | 不负责 3D 分子渲染 | 商业 / 开源 / 开源 |

### 12.2 场景推荐组合

| 场景 | 推荐链路 |
| --- | --- |
| 小分子 ESP 图（论文） | Gaussian → fchk → Multiwfn（density + totesp）→ **MolecuLens**（`--cmap rwb`，固定 ±45） |
| 轨道形状 + 能级图 | Gaussian/ORCA → Multiwfn/orca_plot 出 cube → **MolecuLens**（iso 0.02）+ Origin（能级） |
| 固体 DOS / 能带 | VASP → VASPKIT/pymatgen → gnuplot / Origin（E_F 对齐 0） |
| 弱相互作用（NCI/IGMH） | Gaussian → Multiwfn 主功能 20 → Multiwfn 出散点 + 等值面；MolecuLens 仅作等值面预览 |
| 反应能垒 / IRC | Gaussian/ORCA → cclib 解析 → Origin；几何变化转 DCD → **MolecuLens** 播放 |
| 快速检查 cube 是否合理 | **MolecuLens** 打开 cube 看网格/范围/等值面，再决定是否进 Multiwfn 深挖 |

---

## 13 出图规范

### 13.1 等值面取值表（经验起点，必须按体系调）

| 图类型 | 常用 iso（a.u.） | 备注 |
| --- | ---: | --- |
| 电子密度（vdW 面） | **0.001** | MolecuLens 默认；更外扩 0.0005 |
| 分子轨道 | **0.02–0.05** | 同组比较必须同一 iso |
| 电荷密度差 CDD | **±0.005** | 正负各一层；范围 0.002–0.01 |
| 形变密度 | ±0.005 | 同 CDD |
| 自旋密度 | **0.005** | 自由基可降到 0.001–0.002 |
| ELF | 0.5–0.8 | 看壳层/孤对用 0.7 左右 |
| LOL | 0.5–0.6 | 与 ELF 互补 |
| NCI（RDG） | s = 0.5 | 配合 `sign(λ2)ρ` 着色与 δg 散点 |

### 13.2 色标

- **有符号量**（ESP、CDD、自旋密度、形变密度）用**发散型**：`bwr` / `coolwarm` / `rwb`，中点为 0；`reference` 只用于复刻 GXNU 风格；
- **无量纲/幅度量**（DOS、ELF、LOL）用**顺序型**：`viridis` / `turbo`；
- **色盲友好**优先 `viridis`、`coolwarm`、`bwr`；`jet` 在红绿色盲下易混淆，能不用就不用（MolecuLens 保留它是为了兼容旧图）；
- 同一组图**必须固定色标范围**（ESP 尤其），并在图注写「−45/+45 kcal/mol」；否则读者会误读颜色差异；
- 色标条不要裁掉，单位写在色标标题里（kcal/mol）。

### 13.3 分辨率、背景与版面

| 项 | 建议 |
| --- | --- |
| 位图 | 最终尺寸下 **≥300 dpi**；线条/等值线图用 600 dpi |
| 矢量 | 曲线/能级图/DOS 用 PDF/SVG（Origin、matplotlib、gnuplot 都支持） |
| 3D 渲染 | MolecuLens 导出 1×–4× 输出像素：**先 4× 再缩小**，边缘更干净；4K（3840×2160）用于大版面板 |
| 透明背景 | 幻灯片叠底、组合图用；印刷/投稿一般用白底（透明边缘抗锯齿像素会发灰） |
| 字体 | 最终版面 ≥7 pt；同一篇文章统一字体与字号 |
| 标注 | 极值点、色标、HOMO/LUMO、能垒数值都要有单位；不要只靠颜色传达结论 |

### 13.4 可复现

每张图至少记录：软件与版本、泛函/基组、iso 与色标范围、cube 来源命令、是否做过 ZPE/热校正。MolecuLens 侧建议同时保存 `.mlscene`（表示参数与相机），CLI 出图时把完整命令写进图注或补充材料。

**示意图标注可直接随图导出**：MolecuLens「标注」页的水盒、文字标签、箭头（含波浪「光」箭头）与水印会随 PNG 一起栅格化（**8-bit 路径**，按 900 px 图高基准等比放大），示意图不需要后期再拼标注；**16-bit 与 CPU 光追导出不含标注**，透明背景时也会跳过。标注**随场景保存**（`.mlscene` 可选键 `annotations`，旧文件缺键为空；水盒含三斜原始几何往返），但 `settings.json` 不存——复现时保留 `.mlscene` 即可。

---

## 14 MolecuLens 支持度对照与路线图

### 14.1 对照表

| 能力/图类型 | 现状 | 说明 |
| --- | --- | --- |
| ESP 映射表面 | **✅** | GUI「ESP」模块 + CLI `--esp/--density/--iso/--vmin/--vmax/--cmap/--esp-opacity` |
| 体数据等值面（轨道/密度/ELF 等） | **◐** | 单 cube，iso 0.0005–0.5，颜色=数值；**不解析 fchk、不能选轨道** |
| 轨道 ± 相位双色 | **◐** | 正负值落在色标两端，但不是严格的双色相位图 |
| 能级图 / HOMO-LUMO gap | **✗** | 无面板；用 Origin/gnuplot |
| TD-DFT 棒状谱 | **✗** | 同上 |
| DOS / PDOS | **✗** | 用 Multiwfn / VASPKIT |
| 能带结构 | **✗** | 用 VASP + VASPKIT/pymatgen |
| CDD / 形变密度 | **◐（仅显示）** | 相减需外部完成 |
| 自旋密度 | **◐（仅显示）** | 计算需外部完成 |
| NCI / RDG / IGMH | **✗（计算）/ ◐（显示等值面）** | 散点图与 BGR 着色不支持 |
| PES 扫描 / IRC 能量曲线 | **✗** | 几何帧可转 DCD/XTC 播放（◐） |
| ELF / LOL | **◐（仅显示）** | 计算与切片需外部 |
| ESP 切片/等值线 | **✗** | 只有 3D 表面 |
| 原子电荷着色 | **✗** | 配色仅元素/链/残基/BFactor/单色 |
| 表面极值标注 | **✅** | 8 极大 + 8 极小、1.2 Å NMS；随 PNG 导出 |
| 表面积/体积统计 | **✗** | 用 Multiwfn `surfanalysis` |
| 动画/视频导出 | **✗** | 逐帧 PNG + 外部合成（路线图） |
| 轨迹播放 | **✅** | DCD/XTC/TRR，见 03 §4.3 |
| 示意图标注 | **✅** | 水盒/标签/箭头/水印 + 示意图风格；随 PNG 8-bit 导出，随 `.mlscene` 保存（03 §4.7） |
| 平板表示（辅基） | **✅** | 第 14 种样式：每「链+残基」一块彩色方片（≥3 原子），`--repr plate`；辅基需按残基/链配色（02 §4.2、04 §5.12） |
| 单文件场景分发 | **✅** | `.mlscene` 链接式/嵌入式，见 03 §4.4 |
| 导出 | **✅** | 1×–4×、4K、透明背景；位深 8/16 bit 走 CLI `--depth`（见 `ESP_SURFACE.md` 与 04 §5.4） |

### 14.2 路线图（只列与本指南相关的项，均**未实现**）

1. `.fchk` / `.log` 解析与按轨道号选择、± 双等值面与节面显示；
2. DOS/PDOS 与能级图面板；
3. CDD/自旋密度/形变密度的**计算**（目前只能显示外部算好的 cube）；
4. NCI/RDG/IGMH 分析与 `sign(λ2)ρ` 散点；
5. ESP 切片/等值线、表面积统计；
6. 原子电荷着色；
7. 动画/视频导出与 Python 脚本。

> 上述条目与《02 功能介绍书》§9、《01 软件架构书》§13 的 P1–P3 对应；本文不承诺具体版本号。

---

## 15 十个最常见的坑

1. **单位**：cube 坐标是 Bohr，日志键长是 Å；ESP 是 Hartree 还是 kcal/mol 没对齐。
2. **网格不一致**：density 与 potential（或 AB/A/B）不是同一网格 → 表面缺失或全灰。
3. **iso 跨图不可比**：不同图用了不同 iso，形状「看起来差很多」其实只是阈值不同。
4. **自动范围跨分子比较**：每张图各自 ±99.5 分位，颜色不可比；论文组图必须固定 vmin/vmax。
5. **把 ESP 当总能**：ESP 是静电势，不是能量；「能量势」只是口语。
6. **轨道 iso 跨软件比较**：不同程序 cube 归一化不同，0.02 在 A 软件和 B 软件不等价。
7. **CDD 没固定几何/基组/BSSE**：电荷转移被高估或方向反了。
8. **自旋污染**：`<S²>` 不对时自旋密度「好看但不可信」。
9. **NCI 只看等值面不看 δg**：绿斑不等于强相互作用，峰位要用散点确认。
10. **出图分辨率不足**：1× 截图放大到 300 dpi 会糊；用 4×/4K 再缩放，或用矢量图。

---

## 16 与其它文档的关系

| 文档 | 关系 |
| --- | --- |
| `01-软件架构书.md` | 渲染管线、命令流、持久化；§5.4.1 渲染风格 |
| `02-功能介绍书.md` | 能力矩阵、模块参数、已知限制、路线图（§4.4 ESP / §4.5 轨道 / §4.7 分析） |
| `03-使用说明.md` | 分步操作：§4.2 ESP、§4.3 轨迹、§4.4 保存场景、§4.5 渲染风格 |
| `04-测试报告.md` | 实测数据：§5.9 GUI 复验、§5.10 无头回归（4K 659 ms、TRR 第 500 帧 1082 ms） |
| `ESP_SURFACE.md` | ESP 的 CLI 参数与 Multiwfn 流程原始记录 |
| `VERIFICATION.md` | 测试基线（176 例 / 193,708 断言）与已知遗留 |

> 本文只讲「怎么选工具、怎么设参数」；具体按钮位置与操作步骤请看 03，能力边界与路线图请看 02，性能与实测数字请看 04。
