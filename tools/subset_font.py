#!/usr/bin/env python3
"""tools/subset_font.py —— 把 Noto Sans SC 可变字体裁成 UI 子集（OFL 1.1）

为什么需要：
    · ImGui 的 stb_truetype **不应用可变字体轴**，直接用 NotoSansSC-VF.ttf 只会
      拿到默认实例（wght=100 Thin），界面字过细；
    · 全量 CJK 字体 17.7 MB，而界面只用其中一部分码点，仓库/安装包没必要背着。

做法：
    1. `fontTools.varLib.instancer` 把 wght 固定到指定字重（默认 500 Medium）；
    2. 字符集 = `src/app/**/*.cpp|h` 里出现的全部字符（含 i18n 表与注释）
       ∪ ASCII ∪ 拉丁补充/希腊/箭头/数学算子/CJK 标点/全角符号 ∪ GB2312 全字符
       （GB2312 是给用户文件名等运行期文本留的余量）；
    3. `fontTools.subset` 裁剪，保留 name 表与全部布局特性（中文换行/标点需要 GSUB/GPOS）。

用法：
    python tools/subset_font.py \
        --src "C:/Windows/Fonts/NotoSansSC-VF.ttf" \
        --out data/font/NotoSansSC-Medium-subset.ttf --weight 500

输出必须与 data/font/OFL.txt 一起分发（OFL 第 2 条）。子集后的主字体名不含
保留字体名 "Source"，满足 OFL 第 3 条；若换成带保留名的字体（如思源黑体），
实例化后请改 family name 再保存。
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

REPO = pathlib.Path(__file__).resolve().parent.parent
SCAN_DIR = REPO / "src" / "app"

SYMBOL_RANGES = [
    (0x00A0, 0x00FF),   # 拉丁补充：Å × · ° ± 等
    (0x0370, 0x03FF),   # 希腊字母：Δ α β γ λ
    (0x2000, 0x206F),   # 通用标点：– — … “ ” ‘ ’
    (0x2190, 0x21FF),   # 箭头：← → ↑ ↓ ↔
    (0x2200, 0x22FF),   # 数学算子：≈ ≤ ≥ ∑
    (0x2460, 0x24FF),   # 带圈数字
    (0x25A0, 0x25FF),   # 几何图形：▶ ● ■
    (0x2600, 0x26FF),   # 杂项符号：✓ ★
    (0x3000, 0x303F),   # CJK 标点
    (0xFF00, 0xFFEF),   # 全角符号
]


def collect_charset() -> set[str]:
    chars: set[str] = set()
    for pattern in ("*.cpp", "*.h"):
        for path in SCAN_DIR.rglob(pattern):
            chars.update(path.read_text(encoding="utf-8", errors="ignore"))
    chars.update(chr(c) for c in range(0x20, 0x7F))
    for lo, hi in SYMBOL_RANGES:
        chars.update(chr(c) for c in range(lo, hi + 1))
    # GB2312 全字符（6763 汉字 + 区段符号）：覆盖用户文件名等运行期文本
    for hi_byte in range(0xA1, 0xF8):
        for lo_byte in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi_byte, lo_byte]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return {c for c in chars if ord(c) >= 0x20}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Subset Noto Sans SC for the MolecuLens UI")
    ap.add_argument("--src", required=True, help="源字体（建议 NotoSansSC-VF.ttf）")
    ap.add_argument("--out", default=str(REPO / "data/font/NotoSansSC-Medium-subset.ttf"))
    ap.add_argument("--weight", type=int, default=500, help="wght 轴目标值（默认 500 Medium）")
    args = ap.parse_args(argv)

    src = pathlib.Path(args.src)
    out = pathlib.Path(args.out)
    if not src.exists():
        print(f"[subset] 源字体不存在：{src}", file=sys.stderr)
        return 1

    font = TTFont(src, fontNumber=0)
    if "fvar" in font:
        font = instancer.instantiateVariableFont(font, {"wght": args.weight},
                                                 inplace=False, updateFontNames=True)

    wanted = collect_charset()
    cmap = set(font.getBestCmap().keys())
    have = {c for c in wanted if ord(c) in cmap}
    missing = sorted(c for c in wanted if ord(c) not in cmap)
    if missing:
        print(f"[subset] 源字体缺 {len(missing)} 个请求字符，已跳过（例：{''.join(missing[:20])}）")

    opts = subset.Options()
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.layout_features = ["*"]
    opts.notdef_outline = True
    opts.recommended_glyphs = True
    opts.symbol_cmap = True
    opts.legacy_cmap = True
    opts.glyph_names = True

    sub = subset.Subsetter(options=opts)
    sub.populate(unicodes=[ord(c) for c in have])
    sub.subset(font)
    out.parent.mkdir(parents=True, exist_ok=True)
    font.save(out)

    print(f"[subset] {src.name} -> {out} | 字符 {len(have)} | "
          f"字形 {font['maxp'].numGlyphs} | {out.stat().st_size / 1024 / 1024:.2f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
