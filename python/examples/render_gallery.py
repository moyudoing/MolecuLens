#!/usr/bin/env python3
"""绘图全流程示例：单帧、透明、轨迹、ESP、光追、动画、几何导出。

运行（需先构建 moleculens-cli 与 Python 绑定）::

    cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON
    cmake --build build --config Release --target _native moleculens-cli
    $env:PYTHONPATH = "<repo>/build/python"
    python python/examples/render_gallery.py --out-dir .tmp/render_gallery

脚本只是把 ``moleculens.render`` / ``render_animation`` / ``convert`` 串起来，
每个产物都可单独复现；命令行参数（rep/style/camera/...）与
``moleculens-cli render --help`` 完全一致。
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import moleculens as ml

# python/examples/render_gallery.py → 仓库根
ROOT = Path(__file__).resolve().parents[2]


def _timed(label: str, func, *args, **kwargs):
    started = time.perf_counter()
    path = func(*args, **kwargs)
    elapsed = time.perf_counter() - started
    size_kb = Path(path).stat().st_size / 1024.0
    print(f"[{label}] {Path(path).name}  {size_kb:8.1f} KiB  {elapsed:5.2f} s")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description="MolecuLens Python 绘图全流程示例")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=ROOT / ".tmp" / "render_gallery",
        help="产物目录（默认 .tmp/render_gallery，已被 .gitignore 忽略）",
    )
    parser.add_argument("--width", type=int, default=800)
    parser.add_argument("--height", type=int, default=600)
    parser.add_argument(
        "--cli",
        type=Path,
        default=None,
        help="moleculens-cli 路径（默认自动查找 build/src/cli/<Config>/）",
    )
    args = parser.parse_args()

    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cli = ml.cli_path(args.cli)
    print(f"CLI: {cli}")
    print(f"OUT: {out}")

    size = (args.width, args.height)
    data = ROOT / "data"
    esp_dir = ROOT / "datasets" / "esp" / "glycine"

    _timed(
        "1 publication",
        ml.render,
        data / "EPI.pdb",
        out / "01_publication_ballstick.png",
        rep="ballstick",
        style="publication",
        size=size,
        executable=cli,
    )
    _timed(
        "2 transparent",
        ml.render,
        data / "EPI.pdb",
        out / "02_transparent_stick.png",
        rep="stick",
        coloring="element",
        background="transparent",
        size=size,
        executable=cli,
    )
    _timed(
        "3 trajectory",
        ml.render,
        data / "md.gro",
        out / "03_trajectory_frame25.png",
        rep="wireframe",
        trajectory=data / "md.xtc",
        frame=25,
        style="publication",
        size=size,
        executable=cli,
    )
    _timed(
        "4 esp",
        ml.render,
        esp_dir / "glycine_density.cube",
        out / "04_esp_coolwarm.png",
        esp=esp_dir / "glycine_esp.cube",
        cmap="coolwarm",
        vmin=-40,
        vmax=40,
        zoom=0.65,
        size=size,
        executable=cli,
    )
    _timed(
        "5 raytrace",
        ml.render,
        data / "EPI.pdb",
        out / "05_raytrace_cpu.png",
        rep="ballstick",
        ray_trace=1,
        rt_backend="cpu",
        style="soft",
        size=(args.width // 2, args.height // 2),
        executable=cli,
    )
    _timed(
        "6 animation",
        ml.render_animation,
        data / "md.gro",
        out / "06_trajectory.gif",
        trajectory=data / "md.xtc",
        frame_range="0:12",
        fps=12,
        rep="wireframe",
        style="publication",
        size=(args.width // 2, args.height // 2),
        executable=cli,
    )
    _timed(
        "7 export",
        ml.convert,
        data / "EPI.pdb",
        out / "07_epi.obj",
        rep="ballstick",
        executable=cli,
    )

    print(f"\n{len(list(out.iterdir()))} files written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
