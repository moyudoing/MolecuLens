# MolecuLens Python 数据案例报告

- 生成时间：2026-10-23 00:24:13
- 数据集目录：`datasets/`
- CLI：`moleculens-cli.exe`

## 数据层

| 案例 | 说明 | 原子 | 键 | 帧 | 组成 |
| --- | --- | ---: | ---: | ---: | --- |
| `1CRN` | crambin 蛋白 | 327 | 3 | 1 | C202 N55 O64 S6 |
| `1UBQ` | 泛素 | 660 | 608 | 1 | C378 N105 O176 S1 |
| `1BNA` | B-DNA 十二聚体 | 566 | 545 | 1 | C232 N92 O220 P22 |
| `4HHB` | 人脱氧血红蛋白 | 4779 | 204 | 1 | C2954 N780 O1027 P2 S12 Xx4 |
| `1L2Y` | Trp-cage 微蛋白（NMR 38 模型） | 304 | 310 | 38 | C98 H150 N27 O29 |
| `1HVR` | HIV-1 蛋白酶 + XK2 抑制剂 | 1890 | 72 | 1 | C1017 H330 N262 O275 S6 |
| `1U19` | 牛视紫红素 + 视黄醛 | 5948 | 418 | 1 | C3913 N856 O1110 S56 Xx13 |
| `aspirin` | 阿司匹林 | 21 | 21 | 1 | C9 H8 O4 |
| `caffeine` | 咖啡因 | 24 | 25 | 1 | C8 H10 N4 O2 |
| `ibuprofen` | 布洛芬 | 33 | 33 | 1 | C13 H18 O2 |
| `ethanol` | 乙醇 | 9 | 8 | 1 | C2 H6 O1 |
| `benzene` | 苯 | 12 | 12 | 1 | C6 H6 |
| `glucose` | 葡萄糖 | 24 | 24 | 1 | C6 H12 O6 |
| `resveratrol` | 白藜芦醇 | 29 | 30 | 1 | C14 H12 O3 |
| `md_1u19` | 视紫红素 MD 轨迹拓扑 | 5547 | 5284 | 1 | C1870 H2744 N424 O483 S26 |

## 选择语言

| 查询 | 命中原子 | 表达式 |
| --- | ---: | --- |
| 1HVR 配体 XK2 | 46 | `resname XK2` |
| 1HVR 结合口袋（6 Å） | 260 | `within 6 of resname XK2` |
| 1U19 视黄醛 RET | 40 | `resname RET` |
| 1U19 糖链 | 145 | `resname NAG or resname MAN or resname BMA` |
| 1L2Y NMR 模型数 | 38 | `frame_count` |

## 渲染产物

| 文件 | 大小 |
| --- | ---: |
| `1hvr_ligand.png` | 101,107 B |
| `1l2y_cartoon.png` | 43,570 B |
| `md_frame25.png` | 400,511 B |
| `glycine_esp.png` | 173,831 B |

## 复现

```powershell
cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON
cmake --build build --config Release --target _native moleculens-cli
$env:PYTHONPATH = "$PWD\build\python"
python python/examples/dataset_tour.py --out-dir .tmp/dataset_tour
```
