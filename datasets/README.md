# MolecuLens 示例数据集清单（datasets/）

本目录存放 MolecuLens 文档与演示所用的**可公开分发**分子数据，全部来自官方来源，未经格式重排、未修改坐标或化学结构。

- 采集日期：**2026-10-22**
- 文件总数：**12**，合计 **4,501,578 字节（4.29 MiB）**
- 覆盖范围：蛋白质/核酸结构、小分子 3D 构象、分子动力学轨迹、ESP 表面演示夹具
- 本清单中的每个原子数/键数/帧数均由 `moleculens-cli info` 实测得到，未手写估计值

> 关于许可：本项目的 **Apache-2.0** 仅覆盖 MolecuLens 自身代码与本仓库自制的夹具文件；第三方数据保留其原始许可，见 [§8 许可与授权范围](#8-许可与授权范围)。

---

## 1. 目录总览

```
datasets/
├── README.md                      本清单
├── pdb/                           蛋白质与核酸结构（RCSB PDB）
│   ├── 1CRN.pdb                   crambin，327 原子
│   ├── 1UBQ.pdb                   ubiquitin，660 原子
│   ├── 1BNA.pdb                   B-DNA 十二聚体，566 原子
│   └── 4HHB.pdb                   脱氧血红蛋白，4779 原子
├── pubchem/                       小分子 3D 构象（PubChem，SDF V2000）
│   ├── aspirin_2244.sdf           阿司匹林，CID 2244
│   ├── caffeine_2519.sdf          咖啡因，CID 2519
│   └── ibuprofen_3672.sdf         布洛芬，CID 3672
├── md/                            分子动力学轨迹（MolSysMT，MIT）
│   ├── md_1u19.gro                拓扑/坐标（5547 原子）
│   └── md_1u19.xtc                轨迹（5547 原子 × 51 帧，20 ps/帧）
└── esp/glycine/                   ESP 表面演示夹具（项目自制，合成数据）
    ├── glycine.pdb                甘氨酸结构，10 原子
    ├── glycine_esp.cube           合成 ESP 立方网格（54×42×37）
    └── glycine_density.cube       合成电子密度立方网格（54×42×37）
```

| 家族 | 路径 | 文件数 | 字节 | 上游许可 |
| --- | --- | ---: | ---: | --- |
| 蛋白质/核酸结构 | `pdb/` | 4 | 678,375 | wwPDB 公开数据 |
| 小分子 3D 构象 | `pubchem/` | 3 | 13,799 | 公有领域（PubChem） |
| 分子动力学轨迹 | `md/` | 2 | 1,457,188 | MIT（MolSysMT 仓库） |
| ESP 演示夹具 | `esp/glycine/` | 3 | 2,352,216 | 本项目 Apache-2.0（合成数据） |
| **合计** | | **12** | **4,501,578** | |

---

## 2. 蛋白质与核酸结构 —— RCSB PDB

**来源**：RCSB PDB 官方静态下载端点，HTTPS

```
https://files.rcsb.org/download/1CRN.pdb
https://files.rcsb.org/download/1UBQ.pdb
https://files.rcsb.org/download/1BNA.pdb
https://files.rcsb.org/download/4HHB.pdb
```

**获取日期**：2026-10-22
**许可/使用条款**：PDB 条目为 wwPDB 公开数据，可自由下载、使用与再分发（RCSB PDB 数据政策见 <https://www.rcsb.org/pages/policies>）。本清单不为其主张任何额外许可；原始文件逐字节保存，未做重排。

| 文件 | 条目名称（TITLE） | 分辨率 | 原子数 | 字节 | SHA-256 |
| --- | --- | ---: | ---: | ---: | --- |
| `pdb/1CRN.pdb` | WATER STRUCTURE OF A HYDROPHOBIC PROTEIN AT ATOMIC RESOLUTION（crambin） | 1.50 Å | 327 | 49,491 | `42199a30a0701864a2a5cc76cd7f35cc544cd0e65fbcf63e03c166543249b811` |
| `pdb/1UBQ.pdb` | STRUCTURE OF UBIQUITIN REFINED AT 1.8 ANGSTROMS RESOLUTION | 1.80 Å | 660 | 78,570 | `d4a6812d8951cf6594e6a0763f089e35f5a80b62acb3c117b2c5565228a7b161` |
| `pdb/1BNA.pdb` | STRUCTURE OF A B-DNA DODECAMER. CONFORMATION AND DYNAMICS | 1.90 Å | 566 | 76,464 | `df42f1506792f191b957227b061360652adcf6f813eb69d9ec553067ea584670` |
| `pdb/4HHB.pdb` | THE CRYSTAL STRUCTURE OF HUMAN DEOXYHAEMOGLOBIN AT 1.74 ANGSTROMS RESOLUTION | 1.74 Å | 4779 | 473,850 | `abf382e0fd84b53bd4c08b373d37c0d7fe9fbe1a9a9c3851b2f5f2fbbe627101` |

**引用/署名**（建议在文档中注明条目 ID 与下列经典文献）：

- **1CRN** — Crambin, X-ray, 1981。条目页面 <https://www.rcsb.org/structure/1CRN>；文献：Teeter M.M., Hendrickson W.A., *J. Mol. Biol.* (1981)。
- **1UBQ** — Ubiquitin, X-ray 1.8 Å, 1987。条目页面 <https://www.rcsb.org/structure/1UBQ>；文献：Vijay-Kumar S., Bugg C.E., Cook W.J., *J. Mol. Biol.* **194**:531 (1987)。
- **1BNA** — B-DNA dodecamer (CGCGAATTCGCG), X-ray 1.9 Å, 1981。条目页面 <https://www.rcsb.org/structure/1BNA>；文献：Drew H.R., Dickerson R.E., *J. Mol. Biol.* **151**:535 (1981)。
- **4HHB** — Human deoxyhaemoglobin, X-ray 1.74 Å, 1984。条目页面 <https://www.rcsb.org/structure/4HHB>；文献：Fermi G., Perutz M.F., Shaanan B., Fourme R., *J. Mol. Biol.* **175**:159 (1984)。

---

## 3. 小分子 3D 构象 —— PubChem PUG-REST

**来源**：PubChem PUG-REST 官方接口（3D 构象记录），HTTPS

```
https://pubchem.ncbi.nlm.nih.gov/rest/pug/compound/cid/2244/record/SDF?record_type=3d
https://pubchem.ncbi.nlm.nih.gov/rest/pug/compound/cid/2519/record/SDF?record_type=3d
https://pubchem.ncbi.nlm.nih.gov/rest/pug/compound/cid/3672/record/SDF?record_type=3d
```

**获取日期**：2026-10-22
**许可/使用条款**：PubChem 数据由美国国立卫生研究院（NIH）提供，属**公有领域**（public domain），可自由使用；PubChem 仅要求在使用时适当引用（见 <https://pubchem.ncbi.nlm.nih.gov/docs/programmatic-access>）。记录为 PubChem 生成的 3D 构象（OpenEye 生成），**不是**实验晶体结构。

| 文件 | 化合物 | CID | 分子式 | 原子数 | 字节 | SHA-256 |
| --- | --- | ---: | --- | ---: | ---: | --- |
| `pubchem/aspirin_2244.sdf` | 阿司匹林（acetylsalicylic acid） | 2244 | C9H8O4 | 21 | 4,055 | `e42bf39bc0f0d458cb22b867f0f0183da5a443d8273997dbea9aaa25f4a70ffd` |
| `pubchem/caffeine_2519.sdf` | 咖啡因（caffeine） | 2519 | C8H10N4O2 | 24 | 3,950 | `1b7b9ffbbd7ad968e4181480e4e2899456e42df96de9a19734a0d66c2c4f35b1` |
| `pubchem/ibuprofen_3672.sdf` | 布洛芬（ibuprofen） | 3672 | C13H18O2 | 33 | 5,794 | `6f8165c677672bd5e6452f74a232bd3bb6885a1be0fd73eb3da04f52d5b4e4c9` |

**关于标题行（如实说明）**：PubChem 的 SDF 记录按惯例把 **CID 数字**写在第一行标题行（本目录三个文件的首行分别是 `2244`、`2519`、`3672`），**不含化合物名称**；化合物名由文件名与 CID 共同标识。为保持与上游逐字节一致（便于 SHA-256 复核），此处未改写标题行。

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

> ⚠️ **溯源说明（如实标注）**：上游**没有**为该轨迹单独提供数据 DOI 或独立许可声明，也没有随包的生成脚本（`molsysmt/data/_make/` 中无 `md_1u19.py`）。因此本清单只主张"该文件来自 MIT 许可的 MolSysMT 仓库"，不主张其具有独立的数据 DOI。文件名与 MolSysMT 测试用例一致，对应 **PDB 1U19（牛视紫红质 rhodopsin）** 体系；1U19 结构本身属 wwPDB 公开数据。若后续需要严格可追溯的引用链，建议替换为带 DOI 的轨迹数据集。

| 文件 | 角色 | 原子数 | 帧数 | 帧间隔 | 字节 | SHA-256 |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| `md/md_1u19.gro` | 拓扑 + 首帧坐标（GROMACS .gro） | 5547 | 1 | — | 382,788 | `8eec93cb0b45c43abc50ec40e49f983e2c2484671077777db19bf7340c068c76` |
| `md/md_1u19.xtc` | 轨迹（GROMACS .xtc） | 5547 | 51 | 20 ps | 1,074,400 | `8279bc6723a4b2e70d6ef3e83fbbcaeb3f981d80546ef4195b7b1d7ba5815af1` |

- 两个文件的原子数**一致（5547）**，可直接配对使用；`moleculens-cli info` 对 `.xtc` 直接读出 `帧数: 51`、`时间步: 20.000 ps`。
- 体系为全原子（含氢）蛋白体系，`moleculens-cli info` 报告 29 个结构、349 个实例；残基名中包含 `ACE`、`LRT`、`YPL` 等非标准条目，无显式水分子（残基名列表由 `.gro` 原文提取）。
- **已知限制**：该 `.gro` 拓扑下 `--repr cartoon` 输出**空白图**（不带轨迹时同样空白），请改用 `--repr stick` / `ballstick`（见 §7）。这是拓扑来源差异导致的表示限制，不是数据损坏。

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
| `esp/glycine/glycine_esp.cube` | 合成 ESP 网格（Hartree） | 10（头部原子） | 54×42×37 | 1,175,708 | `8472913120757b581a0ce0ca0e8ff5800ef257804e91abb006b94e7f957abe89` |
| `esp/glycine/glycine_density.cube` | 合成电子密度网格（a.u.） | 10（头部原子） | 54×42×37 | 1,175,711 | `539f819b2ab17c05d7b673dddf3939f338fa9df46236c610f0d0b2f89023e028` |

> `moleculens-cli info` 对 cube 文件只导入头部原子（坐标由 Bohr 换算为 Å），并给出告警"体积数据仅用于 ESP 表面可视化"——这是预期行为。

---

## 6. 校验结果（`moleculens-cli info` 实测）

所有 12 个文件均已用 `build\src\cli\Release\moleculens-cli.exe info <file>` 验证可读。下表为原始输出中的数值：

| 文件 | 结构数 | 实例数 | 原子数 | 键数 | 帧数 | 告警 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `pdb/1CRN.pdb` | 1 | 1 | **327** | 3 | 1 | — |
| `pdb/1UBQ.pdb` | 1 | 1 | **660** | 608 | 1 | — |
| `pdb/1BNA.pdb` | 1 | 1 | **566** | 545 | 1 | — |
| `pdb/4HHB.pdb` | 1 | 1 | **4779** | 204 | 1 | — |
| `pubchem/aspirin_2244.sdf` | 1 | 1 | **21** | 21 | 1 | 1（SD 属性块） |
| `pubchem/caffeine_2519.sdf` | 1 | 1 | **24** | 25 | 1 | 1（SD 属性块） |
| `pubchem/ibuprofen_3672.sdf` | 1 | 1 | **33** | 33 | 1 | 1（SD 属性块） |
| `md/md_1u19.gro` | 29 | 349 | **5547** | 5284 | 1 | — |
| `md/md_1u19.xtc` | — | — | **5547** | — | **51** | — |
| `esp/glycine/glycine.pdb` | 1 | 1 | **10** | 10 | 1 | — |
| `esp/glycine/glycine_esp.cube` | 1 | 1 | 10 | 10 | 1 | 1（体积数据提示） |
| `esp/glycine/glycine_density.cube` | 1 | 1 | 10 | 10 | 1 | 1（体积数据提示） |

说明：
- `1CRN.pdb` 只有 3 条键，是因为该 PDB 文件未提供完整的 `CONECT` 记录，属原始文件特征（不影响卡通/棒状按残基连线渲染）。
- 上表原子数与各文件 `ATOM`/`HETATM` 记录条数逐一核对一致（327 / 660 / 566 / 4779）。
- SDF 告警与 cube 告警均为**预期提示**，不影响结构读取与渲染。

---

## 7. 快速使用（命令）

以下命令均在**仓库根目录**（`F:\MolecuLens`）下执行，输出建议放在 `out\`（已被 `.gitignore` 忽略）。

**查看结构信息**

```bat
build\src\cli\Release\moleculens-cli.exe info datasets\pdb\1CRN.pdb
build\src\cli\Release\moleculens-cli.exe info datasets\pubchem\aspirin_2244.sdf
build\src\cli\Release\moleculens-cli.exe info datasets\md\md_1u19.gro
```

**① 蛋白质：卡通表示（Cartoon）**

```bat
build\src\cli\Release\moleculens-cli.exe render datasets\pdb\1CRN.pdb ^
  --repr cartoon --size 1600 --height 1200 --out out\demo_1crn_cartoon.png
```

**② 蛋白质/核酸：棒状表示（Stick）**

```bat
build\src\cli\Release\moleculens-cli.exe render datasets\pdb\1UBQ.pdb ^
  --repr stick --size 1600 --height 1200 --out out\demo_1ubq_stick.png

build\src\cli\Release\moleculens-cli.exe render datasets\pdb\1BNA.pdb ^
  --repr stick --size 1600 --height 1200 --out out\demo_1bna_stick.png
```

**③ 小分子：球棍表示（Ball & Stick，SDF 直接渲染）**

```bat
build\src\cli\Release\moleculens-cli.exe render datasets\pubchem\aspirin_2244.sdf ^
  --repr ballstick --size 1200 --height 900 --out out\demo_aspirin.png
```

**④ 轨迹帧：拓扑 + 轨迹（指定帧 / 末帧 / 导出动画）**

```bat
REM 第 0 帧（注意：.gro 拓扑请用 stick/ballstick，不要用 cartoon）
build\src\cli\Release\moleculens-cli.exe render datasets\md\md_1u19.gro ^
  --traj datasets\md\md_1u19.xtc --frame 0 --repr stick ^
  --size 1280 --height 960 --out out\demo_md_frame0.png

REM 末帧（--frame -1）
build\src\cli\Release\moleculens-cli.exe render datasets\md\md_1u19.gro ^
  --traj datasets\md\md_1u19.xtc --frame -1 --repr stick ^
  --size 1280 --height 960 --out out\demo_md_last.png

REM 导出整段轨迹动画（mp4/webm/gif/apng 由扩展名决定）
build\src\cli\Release\moleculens-cli.exe render datasets\md\md_1u19.gro ^
  --traj datasets\md\md_1u19.xtc --anim out\demo_md.gif --anim-fps 15 ^
  --size 800 --height 600
```

**⑤ ESP 表面：结构 + ESP cube + 密度 cube**

```bat
build\src\cli\Release\moleculens-cli.exe render datasets\esp\glycine\glycine.pdb ^
  --esp datasets\esp\glycine\glycine_esp.cube ^
  --density datasets\esp\glycine\glycine_density.cube ^
  --repr stick --size 1280 --height 960 --out out\demo_esp_glycine.png
```

可选参数：`--iso 0.001`（密度等值面阈值）、`--vmin/--vmax` 配合 `--no-auto-range` 固定色标范围、`--cmap rwb` 等切换色标、`--esp-opacity 0.85` 调整表面透明度。

**冒烟渲染产物**（本次验证生成，位于 `_testtmp\`，该目录不入库）：

| 家族 | 产物 | 结果 |
| --- | --- | --- |
| PDB 卡通 | `_testtmp\pdb_1crn_cartoon.png` | ✅ |
| PDB 棒状 | `_testtmp\pdb_1ubq_stick.png`、`_testtmp\pdb_1bna_stick.png` | ✅ |
| PDB 大体系 | `_testtmp\pdb_4hhb_cartoon.png` | ✅ |
| SDF 球棍 | `_testtmp\sdf_aspirin.png` | ✅ |
| 轨迹帧 | `_testtmp\md_smoke.png`（第 0 帧）、`_testtmp\md_stick_f0.png`、`_testtmp\md_stick_f25.png`、`_testtmp\md_stick_flast.png` | ✅ 帧间图像互不相同 |
| ESP 表面 | `_testtmp\esp_glycine.png` | ✅ 色标与等值面正常 |

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
- `md/` 的 MIT 许可来自仓库级 `LICENSE`，上游未提供独立数据 DOI（见 §4 溯源说明）。

---

## 9. 已知问题与注意事项

1. **SDF 标题行是 CID 而非化合物名**（PubChem 惯例）。文件与上游逐字节一致；名称见文件名与 §3 表格。若演示需要显示化合物名，建议在 UI 层用文件名映射，而不是改写原始 SDF。
2. **SDF 尾部属性块触发一条告警**：`unsupported SDF/MOL version (only V2000 supported), record skipped: '> <PUBCHEM_COMPOUND_CID>'`。分子解析正常（原子数 21/24/33 与分子式吻合）。如需消除告警，可在 `M  END` 处截断属性块（但会失去与上游的字节一致性，故本目录未做截断）。
3. **`.gro` 拓扑不支持 `--repr cartoon`**：对 `md/md_1u19.gro` 使用卡通表示会得到空白图（无轨迹时同样如此），请改用 `--repr stick` 或 `ballstick`。PDB 输入的卡通表示正常。
4. **`1CRN.pdb` 仅有 3 条键**：原始 PDB 缺少完整 `CONECT` 记录，属文件特征。
5. **ESP cube 是合成数据**：仅供管线验证，禁止用于科学结论（见 §5）。
6. **入库注意（给维护者）**：仓库 `.gitignore` 原有全局 `*.pdb` 规则（本意是排除调试符号），现已补充 `!datasets/**/*.pdb` 例外，`datasets/pdb/*.pdb` 与 `datasets/esp/glycine/glycine.pdb` 可正常跟踪。
7. **禁用词扫描**：`datasets/` 下所有**文本文件**（`.md/.pdb/.sdf/.gro/.cube`）经不区分大小写的子串扫描确认不含项目禁用词。二进制轨迹 `.xtc` 为压缩数据，其中可能偶然出现与禁用词相同的三字节组合（属噪声，非文本），建议扫描时限定文本扩展名，例如：
   `Get-ChildItem datasets -Recurse -File -Include *.md,*.pdb,*.sdf,*.gro,*.cube | Select-String -Pattern '<禁用词>' -SimpleMatch`
8. **总量**：4.29 MiB，远低于 40 MB 预算；未包含任何超大数据。

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
