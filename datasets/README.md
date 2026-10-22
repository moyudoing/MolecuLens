# MolecuLens 示例数据集清单（datasets/）

本目录存放 MolecuLens 文档、Python 示例与回归测试所用的**可公开分发**分子数据，全部来自官方来源，未经格式重排，未修改坐标或化学结构。

- 采集日期：**2026-10-22**；数据案例扩充：**2026-10-23**
- 数据文件：**19 个**，合计 **6,237,240 字节（5.95 MiB）**（另有本清单）
- 覆盖范围：蛋白质 / 核酸晶体与 NMR 结构、蛋白-配体复合物、小分子 3D 构象、分子动力学轨迹、ESP 表面演示夹具
- 验证口径：每个结构文件均由 `moleculens-cli info` 与 Python `moleculens.load` 双重读取；原子数/键数/帧数由程序实测，不手写估计

> 许可边界：本项目的 **Apache-2.0** 仅覆盖 MolecuLens 自身代码、文档与 `datasets/esp/glycine/` 下自制的合成夹具；第三方数据保留原始许可，见 [§8](#8-许可与授权范围)。

---

## 1. 目录总览

```text
datasets/
├── README.md                       本清单
├── pdb/                            蛋白质与核酸结构（RCSB PDB，7 个）
│   ├── 1CRN.pdb                    crambin，327 原子
│   ├── 1UBQ.pdb                    泛素，660 原子
│   ├── 1BNA.pdb                    B-DNA 十二聚体，566 原子
│   ├── 4HHB.pdb                    人脱氧血红蛋白，4,779 原子
│   ├── 1L2Y.pdb                    Trp-cage 微蛋白，304 原子 × 38 NMR 模型
│   ├── 1HVR.pdb                    HIV-1 蛋白酶 + XK2 抑制剂，1,890 原子
│   └── 1U19.pdb                    牛视紫红素 + 视黄醛/糖链/Zn，5,948 原子
├── pubchem/                        小分子 3D 构象（PubChem，7 个）
│   ├── aspirin_2244.sdf            阿司匹林，C9H8O4
│   ├── caffeine_2519.sdf           咖啡因，C8H10N4O2
│   ├── ibuprofen_3672.sdf          布洛芬，C13H18O2
│   ├── ethanol_702.sdf             乙醇，C2H6O
│   ├── benzene_241.sdf             苯，C6H6
│   ├── glucose_5793.sdf            葡萄糖，C6H12O6
│   └── resveratrol_445154.sdf      白藜芦醇，C14H12O3
├── md/                             分子动力学轨迹（MolSysMT，MIT）
│   ├── md_1u19.gro                 拓扑/首帧坐标（5,547 原子）
│   └── md_1u19.xtc                 轨迹（5,547 原子 × 51 帧，20 ps/帧）
└── esp/glycine/                    ESP 表面演示夹具（项目自制，合成数据）
    ├── glycine.pdb                 甘氨酸，10 原子
    ├── glycine_esp.cube            合成 ESP 立方网格（96×80×67）
    └── glycine_density.cube        合成电子密度立方网格（96×80×67）
```

| 家族 | 路径 | 文件数 | 字节 | 上游许可 |
| --- | --- | ---: | ---: | --- |
| 蛋白质 / 核酸结构 | `pdb/` | 7 | 2,400,030 | wwPDB 公开数据 |
| 小分子 3D 构象 | `pubchem/` | 7 | 27,806 | 公有领域（PubChem） |
| 分子动力学轨迹 | `md/` | 2 | 1,457,188 | MIT（MolSysMT 仓库） |
| ESP 演示夹具 | `esp/glycine/` | 3 | 2,352,216 | 本项目 Apache-2.0（合成数据） |
| **合计** | | **19** | **6,237,240** | |

---

## 2. 蛋白质与核酸结构 —— RCSB PDB

**来源**：RCSB PDB 官方静态下载端点，HTTPS

```
https://files.rcsb.org/download/<PDB_ID>.pdb
```

**获取日期**：2026-10-22（1L2Y / 1HVR / 1U19 为 2026-10-23 追加）
**许可/使用条款**：PDB 条目为 wwPDB 公开数据，可自由下载、使用与再分发（RCSB PDB 数据政策见 <https://www.rcsb.org/pages/policies>）。本清单不为其主张任何额外许可；原始文件逐字节保存，未做重排。

| 文件 | PDB | 说明 | 方法 / 分辨率 | 原子 | 键 | 帧 | 字节 | SHA-256 |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| `pdb/1CRN.pdb` | 1CRN | crambin | X-ray / 1.50 Å | 327 | 3 | 1 | 49,491 | `42199a30a0701864a2a5cc76cd7f35cc544cd0e65fbcf63e03c166543249b811` |
| `pdb/1UBQ.pdb` | 1UBQ | 泛素 | X-ray / 1.80 Å | 660 | 608 | 1 | 78,570 | `d4a6812d8951cf6594e6a0763f089e35f5a80b62acb3c117b2c5565228a7b161` |
| `pdb/1BNA.pdb` | 1BNA | B-DNA 十二聚体 | X-ray / 1.90 Å | 566 | 545 | 1 | 76,464 | `df42f1506792f191b957227b061360652adcf6f813eb69d9ec553067ea584670` |
| `pdb/4HHB.pdb` | 4HHB | 人脱氧血红蛋白 | X-ray / 1.74 Å | 4,779 | 204 | 1 | 473,850 | `abf382e0fd84b53bd4c08b373d37c0d7fe9fbe1a9a9c3851b2f5f2fbbe627101` |
| `pdb/1L2Y.pdb` | 1L2Y | Trp-cage 微蛋白（TC5b） | NMR / — | 304 | 310 | **38** | 959,202 | `5d1bbb545a312dfff1ae1e64b6d8addecb2f561ddc4011aeb5bee9d1dfcd4438` |
| `pdb/1HVR.pdb` | 1HVR | HIV-1 蛋白酶 + XK2 抑制剂 | X-ray / 1.80 Å | 1,890 | 72 | 1 | 190,431 | `c8d3f238d3269a66823454d4681b71467b4677a18751ce094047961620b338cb` |
| `pdb/1U19.pdb` | 1U19 | 牛视紫红素（含 RET / 糖链 / Zn） | X-ray / 2.20 Å | 5,948 | 418 | 1 | 572,022 | `7cffa0b8dd5f9c680e7c86d528212cce60a8f08490450c21d3f0e8dc1a7c8924` |

**引用/署名**（建议在文档中注明 PDB ID 与下列经典文献）：

- **1CRN** — Crambin, X-ray, 1981。Teeter M.M., Hendrickson W.A., *J. Mol. Biol.* (1981)。
- **1UBQ** — Ubiquitin, X-ray 1.8 Å, 1987。Vijay-Kumar S., Bugg C.E., Cook W.J., *J. Mol. Biol.* **194**:531 (1987)。
- **1BNA** — B-DNA dodecamer (CGCGAATTCGCG), X-ray 1.9 Å, 1981。Drew H.R., Dickerson R.E., *J. Mol. Biol.* **151**:535 (1981)。
- **4HHB** — Human deoxyhaemoglobin, X-ray 1.74 Å, 1984。Fermi G., Perutz M.F., Shaanan B., Fourme R., *J. Mol. Biol.* **175**:159 (1984)。
- **1L2Y** — Trp-cage TC5b, NMR, 2002。Neidigh J.W., Fesinmeyer R.M., Andersen N.H., *Nat. Struct. Biol.* **9**:425 (2002)。
- **1HVR** — HIV-1 protease with cyclic urea inhibitor XK2, X-ray 1.8 Å, 1994。Lam P.Y.S. et al., *Science* **263**:380 (1994)。
- **1U19** — Bovine rhodopsin, X-ray 2.2 Å, 2000。Palczewski K. et al., *Science* **289**:739 (2000)。

> 说明：`1U19.pdb` 为 X 射线结构，不含氢；13 个原子被解析为未知元素（`Xx`，多为修饰残基/离子），属原始文件特征。`1L2Y.pdb` 含 38 个 NMR MODEL，MolecuLens 将其作为 38 帧读取。

---

## 3. 小分子 3D 构象 —— PubChem PUG-REST

**来源**：PubChem PUG-REST 官方接口（3D 构象记录），HTTPS

```
https://pubchem.ncbi.nlm.nih.gov/rest/pug/compound/cid/<CID>/record/SDF?record_type=3d
```

**获取日期**：2026-10-22（乙醇 / 苯 / 葡萄糖 / 白藜芦醇为 2026-10-23 追加）
**许可/使用条款**：PubChem 数据由美国国立卫生研究院（NIH）提供，属**公有领域**（public domain），可自由使用；PubChem 仅要求在使用时适当引用（见 <https://pubchem.ncbi.nlm.nih.gov/docs/programmatic-access>）。记录为 PubChem 生成的 3D 构象（OpenEye 生成），**不是**实验晶体结构。

| 文件 | 化合物 | CID | 分子式 | 原子 | 字节 | SHA-256 |
| --- | --- | ---: | --- | ---: | ---: | --- |
| `pubchem/aspirin_2244.sdf` | 阿司匹林（acetylsalicylic acid） | 2244 | C9H8O4 | 21 | 4,055 | `e42bf39bc0f0d458cb22b867f0f0183da5a443d8273997dbea9aaa25f4a70ffd` |
| `pubchem/caffeine_2519.sdf` | 咖啡因（caffeine） | 2519 | C8H10N4O2 | 24 | 3,950 | `1b7b9ffbbd7ad968e4181480e4e2899456e42df96de9a19734a0d66c2c4f35b1` |
| `pubchem/ibuprofen_3672.sdf` | 布洛芬（ibuprofen） | 3672 | C13H18O2 | 33 | 5,794 | `6f8165c677672bd5e6452f74a232bd3bb6885a1be0fd73eb3da04f52d5b4e4c9` |
| `pubchem/ethanol_702.sdf` | 乙醇（ethanol） | 702 | C2H6O | 9 | 1,832 | `dbb3d4fecc2c42a0b2d99148c3c25ff83eb8f8ac81c8dc2914b681cfccb7be16` |
| `pubchem/benzene_241.sdf` | 苯（benzene） | 241 | C6H6 | 12 | 2,245 | `ff2003c2669a4e91d5ffa852f09c0e37c6120f1113dbf10f7600c06c65d993e8` |
| `pubchem/glucose_5793.sdf` | 葡萄糖（glucose） | 5793 | C6H12O6 | 24 | 4,037 | `dff468f6ffbda5edf7beaf2ee28b696967cdae02ba991f7f475560ec24d6c860` |
| `pubchem/resveratrol_445154.sdf` | 白藜芦醇（resveratrol） | 445154 | C14H12O3 | 29 | 5,893 | `ddcd16342c01b376ec703cd646b6594d5ab5838afd7965d2c80e9a5c39f65259` |

**关于标题行（如实说明）**：PubChem 的 SDF 记录按惯例把 **CID 数字**写在第一行标题行（本目录文件的首行是 CID），**不含化合物名称**；化合物名由文件名与 CID 共同标识。为保持与上游逐字节一致（便于 SHA-256 复核），此处未改写标题行。

**已知解析提示**：记录末尾的 SD 属性块（`> <PUBCHEM_...>`）会让 CLI 输出一条告警
`unsupported SDF/MOL version (only V2000 supported), record skipped: '> <PUBCHEM_COMPOUND_CID>'`。
该告警针对的是属性块，**分子本身解析正常**（原子数/键数与分子式一致），渲染不受影响。

---

## 4. 分子动力学轨迹 —— MolSysMT（MIT）

**来源**：`uibcdf/MolSysMT` 仓库（MIT License）内随包分发的测试轨迹

```
https://raw.githubusercontent.com/uibcdf/MolSysMT/main/molsysmt/data/gro/md_1u19.gro
https://raw.githubusercontent.com/uibcdf/MolSysMT/main/molsysmt/data/xtc/md_1u19.xtc
```

**获取日期**：2026-10-22
**许可**：**MIT**。上游 `LICENSE` 原文为 `MIT License / Copyright (c) 2018-2026 Liliana M. Moreno Vargas, Diego Prada Gracia and contributors / UIBCDF Lab at the Mexico City Childrens Hospital Federico Gomez.`，数据文件以该仓库内容形式分发。
**引用**：Moreno Vargas L.M., Prada Gracia D. et al., *MolSysMT* (UIBCDF Lab), <https://github.com/uibcdf/MolSysMT>。

> ⚠️ **溯源说明（如实标注）**：上游**没有**为该轨迹单独提供数据 DOI 或独立许可声明，也没有随包的生成脚本。因此本清单只主张"该文件来自 MIT 许可的 MolSysMT 仓库"，不主张其具有独立的数据 DOI。文件名对应 **PDB 1U19（牛视紫红素 rhodopsin）** 体系；1U19 结构本身属 wwPDB 公开数据。若后续需要严格可追溯的引用链，建议替换为带 DOI 的轨迹数据集。

| 文件 | 角色 | 原子数 | 帧数 | 帧间隔 | 字节 | SHA-256 |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| `md/md_1u19.gro` | 拓扑 + 首帧坐标（GROMACS .gro） | 5,547 | 1 | — | 382,788 | `8eec93cb0b45c43abc50ec40e49f983e2c2484671077777db19bf7340c068c76` |
| `md/md_1u19.xtc` | 轨迹（GROMACS .xtc） | 5,547 | **51** | 20 ps | 1,074,400 | `8279bc6723a4b2e70d6ef3e83fbbcaeb3f981d80546ef4195b7b1d7ba5815af1` |

- 两个文件的原子数**一致（5,547）**，可直接配对使用；`moleculens-cli info` 对 `.xtc` 直接读出 `帧数: 51`、`时间步: 20.000 ps`。
- 体系为全原子（含氢）视紫红素单体，残基名包含 `ACE`、`RET`（视黄醛）等，无显式水分子。
- **已知限制**：该 `.gro` 拓扑下 `--repr cartoon` 输出**空白图**（不带轨迹时同样空白），请改用 `--repr stick` / `ballstick`。这是拓扑来源差异导致的表示限制，不是数据损坏。
- `1U19.pdb`（5,948 原子，X 射线、无氢）与 `md_1u19.gro`（5,547 原子，全原子）**不是同一套原子列表**，不能直接作为同一轨迹的拓扑/结构配对；演示轨迹时请以 `.gro` 为准。

---

## 5. ESP 表面演示夹具 —— glycine（**合成数据**）

**来源**：本仓库内的演示夹具副本

```
_esp/demo/glycine_density.cube   →  datasets/esp/glycine/glycine_density.cube
_esp/demo/glycine_esp.cube       →  datasets/esp/glycine/glycine_esp.cube
data/glycine.pdb                 →  datasets/esp/glycine/glycine.pdb
```

**获取日期**：2026-10-22（自本仓库复制）
**性质**：**合成流水线夹具（synthetic pipeline fixture），不是实验数据，也不是 DFT/量化计算结果。** 两个 cube 文件的第二行自述即写明：

- `glycine_density.cube`：`dim=3 units=bohr; density in a.u. (synthetic, for pipeline testing)`
- `glycine_esp.cube`：`dim=3 units=bohr; ESP in Hartree (synthetic point-charge field)`

其数值由简单点电荷场/解析密度生成，仅用于验证 ESP 着色管线（等值面提取、色标映射、透明度）。**不得**用于任何科学结论或与实验静电势比较。
**许可**：本项目自制，随 MolecuLens 以 **Apache-2.0** 发布。

| 文件 | 说明 | 原子数 | 网格 | 字节 | SHA-256 |
| --- | --- | ---: | --- | ---: | --- |
| `esp/glycine/glycine.pdb` | 甘氨酸 10 原子结构（HETATM，含氢） | 10 | — | 797 | `bd1acf8eab9a6e89a25e10d6fc2b74d1c13e6715d267dbda799093b5ea02d6c4` |
| `esp/glycine/glycine_esp.cube` | 合成 ESP 网格（Hartree） | 10（头部原子） | 96×80×67 | 1,175,708 | `8472913120757b581a0ce0ca0e8ff5800ef257804e91abb006b94e7f957abe89` |
| `esp/glycine/glycine_density.cube` | 合成电子密度网格（a.u.） | 10（头部原子） | 96×80×67 | 1,175,711 | `539f819b2ab17c05d7b673dddf3939f338fa9df46236c610f0d0b2f89023e028` |

> `moleculens-cli info` 对 cube 文件只导入头部原子（坐标由 Bohr 换算为 Å），并给出告警"体积数据仅用于 ESP 表面可视化"——这是预期行为。

---

## 6. 校验结果（CLI + Python 实测）

所有 19 个文件均已用 `moleculens-cli info` 与 Python `moleculens.load` 验证可读。下表为程序实测值：

| 文件 | 原子 | 键 | 帧 | 备注 |
| --- | ---: | ---: | ---: | --- |
| `pdb/1CRN.pdb` | 327 | 3 | 1 | 无完整 CONECT |
| `pdb/1UBQ.pdb` | 660 | 608 | 1 | — |
| `pdb/1BNA.pdb` | 566 | 545 | 1 | 含 80 个结晶水 |
| `pdb/4HHB.pdb` | 4,779 | 204 | 1 | 四聚体 + 血红素 |
| `pdb/1L2Y.pdb` | 304 | 310 | **38** | 38 个 NMR MODEL |
| `pdb/1HVR.pdb` | 1,890 | 72 | 1 | 配体 XK2 46 原子 |
| `pdb/1U19.pdb` | 5,948 | 418 | 1 | RET 40 原子；糖链 145 原子 |
| `pubchem/aspirin_2244.sdf` | 21 | 21 | 1 | SD 属性块告警 |
| `pubchem/caffeine_2519.sdf` | 24 | 25 | 1 | 同上 |
| `pubchem/ibuprofen_3672.sdf` | 33 | 33 | 1 | 同上 |
| `pubchem/ethanol_702.sdf` | 9 | 8 | 1 | 同上 |
| `pubchem/benzene_241.sdf` | 12 | 12 | 1 | 同上 |
| `pubchem/glucose_5793.sdf` | 24 | 24 | 1 | 同上 |
| `pubchem/resveratrol_445154.sdf` | 29 | 30 | 1 | 同上 |
| `md/md_1u19.gro` | 5,547 | 5,284 | 1 | 29 structures / 349 instances |
| `md/md_1u19.xtc` | 5,547 | — | **51** | 20 ps/帧 |
| `esp/glycine/glycine.pdb` | 10 | 10 | 1 | — |
| `esp/glycine/glycine_esp.cube` | 10 | 10 | 1 | 体积数据提示 |
| `esp/glycine/glycine_density.cube` | 10 | 10 | 1 | 体积数据提示 |

Python 回归测试 `python/tests/test_datasets.py` 覆盖上述原子/键/帧数、配体选择与清单一致性；运行方式见 §7。

---

## 7. 快速使用

### 7.1 CLI（无 Python）

```powershell
$cli = "build\src\cli\Release\moleculens-cli.exe"

# 蛋白-配体：卡通 + XK2 球棍
& $cli render datasets\pdb\1HVR.pdb `
    --rep "cartoon:not resname XK2:chain" --rep "ballstick:resname XK2:element" `
    --zoom 1.2 --style publication --bg white --size 1600 --height 1200 --out out\1hvr.png

# 视紫红素：卡通 + 视黄醛
& $cli render datasets\pdb\1U19.pdb `
    --rep "cartoon:not resname HOH:chain" --rep "ballstick:resname RET:element" `
    --zoom 1.2 --style publication --bg white --size 1600 --height 1200 --out out\1u19.png

# NMR 多模型：第 0 帧
& $cli render datasets\pdb\1L2Y.pdb --repr cartoon --frame 0 --zoom 1.2 `
    --style publication --bg white --size 1600 --height 1200 --out out\1l2y.png

# 小分子：球棍
& $cli render datasets\pubchem\resveratrol_445154.sdf --repr ballstick `
    --style publication --bg white --size 1400 --height 1050 --out out\resveratrol.png

# 轨迹第 25 帧
& $cli render datasets\md\md_1u19.gro --traj datasets\md\md_1u19.xtc --frame 25 `
    --repr stick --zoom 1.35 --style publication --bg white `
    --size 1600 --height 1200 --out out\md25.png

# ESP 映射表面
& $cli render datasets\esp\glycine\glycine.pdb `
    --esp datasets\esp\glycine\glycine_esp.cube `
    --density datasets\esp\glycine\glycine_density.cube `
    --repr stick --cmap coolwarm --style publication --bg white `
    --size 1400 --height 1050 --out out\esp.png
```

### 7.2 Python（数据层 + 绘图层）

```powershell
python -m pip install pybind11 pytest pillow
cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON
cmake --build build --config Release --target _native moleculens-cli
$env:PYTHONPATH = "$PWD\build\python"

# 数据层：加载、统计、选择
python -c "import moleculens as ml; m=ml.load('datasets/pdb/1HVR.pdb'); print(m.summary()['atom_count'], len(m.select('resname XK2')))"

# 一键巡览全部数据案例并出图/生成报告
python python\examples\dataset_tour.py --out-dir .tmp\dataset_tour

# 只跑数据层
python python\examples\dataset_tour.py --no-render
```

`dataset_tour.py` 会打印 15 个结构案例的原子/键/帧/组成，演示 `resname`、`within` 选择，并调用 `moleculens.render` / `convert` 生成 PNG、OBJ 与 `report.md`。完整 Python API 见 [`python/README.md`](../python/README.md) 与 [Python 接口与案例](../docs/07-Python接口与案例.md)。

---

## 8. 许可与授权范围

| 数据集 | 上游许可 | 受本项目 Apache-2.0 覆盖？ | 再分发要求 |
| --- | --- | --- | --- |
| `pdb/*.pdb`（RCSB PDB） | wwPDB 公开数据（RCSB PDB 数据政策） | ❌ 否，保留上游条款 | 自由使用；学术使用时引用条目 ID 与原始文献 |
| `pubchem/*.sdf`（PubChem） | 公有领域（US NIH） | ❌ 否 | 自由使用；建议引用 PubChem |
| `md/md_1u19.*`（MolSysMT） | **MIT** | ❌ 否，保留 MIT | 再分发需保留版权声明与 MIT 许可文本 |
| `esp/glycine/*`（项目自制，合成） | **Apache-2.0** | ✅ 是 | 随本项目许可 |

**明确区分：**

- MolecuLens 的 **Apache-2.0** 许可覆盖：本项目源代码、着色器、文档，以及 `datasets/esp/glycine/` 下由本项目**自制**的合成夹具。
- Apache-2.0 **不覆盖**：`datasets/pdb/`、`datasets/pubchem/`、`datasets/md/` 中的第三方数据。这些文件按各自上游许可分发，不得以 Apache-2.0 名义再授权。
- 第三方数据**均为允许再分发的许可**：wwPDB 公开数据、PubChem 公有领域、MIT。本目录**不含** GPL、非商业（NC）、CC-BY-NC/ND 或来源不明的数据。

---

## 9. 已知问题与注意事项

1. **SDF 标题行是 CID 而非化合物名**（PubChem 惯例）。文件与上游逐字节一致；名称见文件名与 §3 表格。
2. **SDF 尾部属性块触发一条告警**：`unsupported SDF/MOL version`。分子解析正常（原子数/键数与分子式吻合），渲染不受影响。
3. **`.gro` 拓扑不支持 `--repr cartoon`**：对 `md/md_1u19.gro` 使用卡通表示会得到空白图，请改用 `--repr stick` / `ballstick`。PDB 输入的卡通表示正常。
4. **`1CRN.pdb` 仅有 3 条键**：原始 PDB 缺少完整 `CONECT` 记录，属文件特征。
5. **`1U19.pdb` 无氢、含 13 个未知元素原子（Xx）**：X 射线结构的原始特征，不影响渲染；与 `md_1u19.gro` 不是同一套原子列表。
6. **`1L2Y.pdb` 是 38 模型 NMR 结构**：MolecuLens 按 38 帧读取，`--frame` 可逐模型查看。
7. **ESP cube 是合成数据**：仅供管线验证，禁止用于科学结论（见 §5）。
8. **入库注意**：`.gitignore` 已补充 `!datasets/**/*.pdb` 例外，结构文件可正常跟踪。
9. **总量**：5.95 MiB，未包含任何超大数据。

---

## 10. 复现方式

按 §2–§5 的 URL 重新下载即可；校验方式：

```powershell
# 大小 + SHA-256（对照本清单表格）
Get-ChildItem datasets -Recurse -File | ForEach-Object {
    "{0}  {1}  {2}" -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.FullName
}

# 若环境缺少 Get-FileHash，可对单文件使用：
certutil -hashfile datasets\pdb\1CRN.pdb SHA256
```

本次采集使用 Python 3.9 `urllib` 按上述官方 HTTPS URL 取回原始字节，未做任何转码或重排；上表 SHA-256 可用于独立复核。
