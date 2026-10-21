# data/ —— 演示与测试夹具

本目录中的结构文件（PDB / GRO / SDF）与轨迹文件用于演示和单元测试。测试通过
`F:/MolecuLens/data/...` 绝对路径引用它们，文件缺失时 `SKIP` 而不是失败（约定见
`tests/entity/trajectory/multiframe_tests.cpp`）。

## 轨迹夹具

| 文件 | 内容 | 来源与许可 |
| --- | --- | --- |
| `traj_multiframe.pdb` | 3 个 MODEL × 4 原子，逐帧沿 +X 平移 1 Å | 本工程自造 |
| `traj_xtc_10f10a.xtc` | XTC，10 原子 × 10 帧；第 f 帧全部原子在 (f,f,f) nm，盒子 20 nm 立方，step=f，time=0.5f | MDAnalysis 测试数据 `xtc_test_only_10_frame_10_atoms.xtc`，**GPL-2.0+**（已获用户确认入库） |
| `md.xtc` | XTC，6452 原子 × 1001 帧，dt=10 ps，立方盒 ≈4.03 nm | 见下方"待确认" |

> **`traj_xtc_10f10a.xtc` 的许可提醒**：该文件取自 GPL-2.0+ 项目 MDAnalysis 的
> 测试数据。它仅作为**测试夹具**存在于本仓库（不与产品代码链接、不随二进制分发），
> 入库已获用户明确确认。若本工程将来改为更宽松的分发许可，需重新评估该文件。

> **待确认**：`md.xtc` 是工作区中既有的真实 GROMACS 轨迹（早于本轮改动就在
> `data/` 下），来源与许可未经核实。XTC 读取器的全集校验用例依赖它；若其来源
> 无法确认，应替换为自造夹具或删除该用例（`xtc_reader_tests.cpp` 中的
> "matches the reference decoder on a real trajectory"）。

## XTC 夹具的期望值从哪来

不是由被测实现自己产出的，而是用 xdrfile **官方实现**独立解码得到（金标准）。
复现步骤见 `other/xdrfile_ref/README.md`。
