#!/usr/bin/env python3
"""数据案例巡览：用 Python 接口加载 datasets/、做选择、出图并生成报告。

运行（需先构建 Python 绑定与 CLI）::

    cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON
    cmake --build build --config Release --target _native moleculens-cli
    $env:PYTHONPATH = "$PWD\\build\\python"
    python python/examples/dataset_tour.py --out-dir .tmp/dataset_tour

脚本演示三件事：

1. **数据层**：``moleculens.load`` 读取 PDB / SDF / GRO，打印原子、键、帧与组成；
2. **选择语言**：在蛋白-配体复合物里按 ``resname`` / ``within`` 取配体与口袋；
3. **绘图层**：``moleculens.render`` / ``convert`` 直接复用 ``moleculens-cli``，
   把同一批数据渲染成出版风格 PNG 与 OBJ，并生成 ``report.md``。

``--out-dir docs/figs_v2/demo/python`` 可把图直接写进文档目录。
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import moleculens as ml

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parents[2]
DATASETS = ROOT / "datasets"

CASES = [
    # key, path, 说明, 期望原子数, 期望帧数
    ("1CRN", DATASETS / "pdb" / "1CRN.pdb", "crambin 蛋白", 327, 1),
    ("1UBQ", DATASETS / "pdb" / "1UBQ.pdb", "泛素", 660, 1),
    ("1BNA", DATASETS / "pdb" / "1BNA.pdb", "B-DNA 十二聚体", 566, 1),
    ("4HHB", DATASETS / "pdb" / "4HHB.pdb", "人脱氧血红蛋白", 4779, 1),
    ("1L2Y", DATASETS / "pdb" / "1L2Y.pdb", "Trp-cage 微蛋白（NMR 38 模型）", 304, 38),
    ("1HVR", DATASETS / "pdb" / "1HVR.pdb", "HIV-1 蛋白酶 + XK2 抑制剂", 1890, 1),
    ("1U19", DATASETS / "pdb" / "1U19.pdb", "牛视紫红素 + 视黄醛", 5948, 1),
    ("aspirin", DATASETS / "pubchem" / "aspirin_2244.sdf", "阿司匹林", 21, 1),
    ("caffeine", DATASETS / "pubchem" / "caffeine_2519.sdf", "咖啡因", 24, 1),
    ("ibuprofen", DATASETS / "pubchem" / "ibuprofen_3672.sdf", "布洛芬", 33, 1),
    ("ethanol", DATASETS / "pubchem" / "ethanol_702.sdf", "乙醇", 9, 1),
    ("benzene", DATASETS / "pubchem" / "benzene_241.sdf", "苯", 12, 1),
    ("glucose", DATASETS / "pubchem" / "glucose_5793.sdf", "葡萄糖", 24, 1),
    ("resveratrol", DATASETS / "pubchem" / "resveratrol_445154.sdf", "白藜芦醇", 29, 1),
    ("md_1u19", DATASETS / "md" / "md_1u19.gro", "视紫红素 MD 轨迹拓扑", 5547, 1),
]


def _fmt_composition(summary: dict) -> str:
    comp = summary.get("composition") or {}
    return " ".join(f"{el}{n}" for el, n in sorted(comp.items()))


def _timed(label: str, func, *args, **kwargs):
    started = time.perf_counter()
    path = func(*args, **kwargs)
    elapsed = time.perf_counter() - started
    print(f"[{label}] {Path(path).name}  {Path(path).stat().st_size / 1024:8.1f} KiB  {elapsed:5.2f} s")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description="MolecuLens Python 数据案例巡览")
    parser.add_argument("--out-dir", type=Path, default=ROOT / ".tmp" / "dataset_tour")
    parser.add_argument("--width", type=int, default=1200)
    parser.add_argument("--height", type=int, default=900)
    parser.add_argument("--no-render", action="store_true", help="只打印数据层结果")
    parser.add_argument("--no-export", action="store_true", help="跳过 OBJ 几何导出")
    parser.add_argument("--cli", type=Path, default=None)
    args = parser.parse_args()

    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    size = (args.width, args.height)
    cli = ml.cli_path(args.cli)
    print(f"CLI: {cli}")
    print(f"OUT: {out}\n")

    # ---------- 1) 数据层：批量加载 ----------
    rows = []
    print(f"{'case':12s} {'atoms':>6s} {'bonds':>6s} {'frames':>6s}  composition")
    for key, path, desc, want_atoms, want_frames in CASES:
        if not path.is_file():
            print(f"{key:12s}  MISSING {path}")
            continue
        mol = ml.load(path)
        summary = mol.summary()
        assert summary["atom_count"] == want_atoms, (key, summary["atom_count"])
        assert summary["frame_count"] == want_frames, (key, summary["frame_count"])
        rows.append((key, path, desc, summary))
        print(
            f"{key:12s} {summary['atom_count']:6d} {summary['bond_count']:6d} "
            f"{summary['frame_count']:6d}  {_fmt_composition(summary)}"
        )

    # ---------- 2) 选择语言：配体 / 口袋 / 多模型 ----------
    hiv = ml.load(DATASETS / "pdb" / "1HVR.pdb")
    ligand = hiv.select("resname XK2")
    pocket = hiv.select("within 6 of resname XK2")
    rhodopsin = ml.load(DATASETS / "pdb" / "1U19.pdb")
    retinal = rhodopsin.select("resname RET")
    glycan = rhodopsin.select("resname NAG or resname MAN or resname BMA")
    trp_cage = ml.load(DATASETS / "pdb" / "1L2Y.pdb")

    selections = [
        ("1HVR 配体 XK2", len(ligand), "resname XK2"),
        ("1HVR 结合口袋（6 Å）", len(pocket), "within 6 of resname XK2"),
        ("1U19 视黄醛 RET", len(retinal), "resname RET"),
        ("1U19 糖链", len(glycan), "resname NAG or resname MAN or resname BMA"),
        ("1L2Y NMR 模型数", trp_cage.frame_count, "frame_count"),
    ]
    print("\n选择语言：")
    for label, count, expr in selections:
        print(f"  {label:24s} {count:5d}   {expr}")

    # ---------- 3) 绘图层：Python 调用 CLI ----------
    rendered: list[Path] = []
    exported: list[Path] = []
    if not args.no_render:
        print("\n渲染（Python -> moleculens-cli）：")
        rendered.append(
            _timed(
                "1HVR ligand",
                ml.render,
                DATASETS / "pdb" / "1HVR.pdb",
                out / "1hvr_ligand.png",
                rep=["cartoon:not resname XK2:chain", "ballstick:resname XK2:element"],
                zoom=1.2,
                style="publication",
                background="white",
                size=size,
                executable=cli,
            )
        )
        rendered.append(
            _timed(
                "1L2Y cartoon",
                ml.render,
                DATASETS / "pdb" / "1L2Y.pdb",
                out / "1l2y_cartoon.png",
                rep="cartoon",
                zoom=1.2,
                style="publication",
                background="white",
                size=size,
                executable=cli,
            )
        )
        rendered.append(
            _timed(
                "MD frame 25",
                ml.render,
                DATASETS / "md" / "md_1u19.gro",
                out / "md_frame25.png",
                rep="stick",
                trajectory=DATASETS / "md" / "md_1u19.xtc",
                frame=25,
                zoom=1.35,
                style="publication",
                background="white",
                size=size,
                executable=cli,
            )
        )
        rendered.append(
            _timed(
                "ESP glycine",
                ml.render,
                DATASETS / "esp" / "glycine" / "glycine_density.cube",
                out / "glycine_esp.png",
                esp=DATASETS / "esp" / "glycine" / "glycine_esp.cube",
                rep="stick",
                cmap="coolwarm",
                vmin=-40,
                vmax=40,
                zoom=0.65,
                style="publication",
                background="white",
                size=size,
                executable=cli,
            )
        )
        if not args.no_export:
            exported.append(
                _timed(
                    "ethanol OBJ",
                    ml.convert,
                    DATASETS / "pubchem" / "ethanol_702.sdf",
                    out / "ethanol.obj",
                    rep="ballstick",
                    executable=cli,
                )
            )

    # ---------- 4) 报告 ----------
    report = [
        "# MolecuLens Python 数据案例报告",
        "",
        f"- 生成时间：{time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"- 数据集目录：`datasets/`",
        f"- CLI：`{cli.name}`",
        "",
        "## 数据层",
        "",
        "| 案例 | 说明 | 原子 | 键 | 帧 | 组成 |",
        "| --- | --- | ---: | ---: | ---: | --- |",
    ]
    for key, path, desc, summary in rows:
        report.append(
            f"| `{key}` | {desc} | {summary['atom_count']} | {summary['bond_count']} | "
            f"{summary['frame_count']} | {_fmt_composition(summary)} |"
        )
    report += ["", "## 选择语言", "", "| 查询 | 命中原子 | 表达式 |", "| --- | ---: | --- |"]
    for label, count, expr in selections:
        report.append(f"| {label} | {count} | `{expr}` |")
    if rendered:
        report += ["", "## 渲染产物", "", "| 文件 | 大小 |", "| --- | ---: |"]
        for path in rendered:
            report.append(f"| `{path.name}` | {path.stat().st_size:,} B |")
    if exported:
        report += ["", "## 几何导出", ""]
        for path in exported:
            report.append(f"- `{path.name}`")
    report += [
        "",
        "## 复现",
        "",
        "```powershell",
        "cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON",
        "cmake --build build --config Release --target _native moleculens-cli",
        '$env:PYTHONPATH = "$PWD\\build\\python"',
        "python python/examples/dataset_tour.py --out-dir .tmp/dataset_tour",
        "```",
        "",
    ]
    report_path = out / "report.md"
    report_path.write_text("\n".join(report), encoding="utf-8")
    print(f"\n报告：{report_path}")
    print(f"产物：{len(rendered) + len(exported)} 个文件 -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
