#!/usr/bin/env python3
"""Moleculens · tools/check_font_glyphs.py —— 检查字体文件是否真有某些字形

用途：与 tools/check_glyph_coverage.py 配套。
    check_glyph_coverage.py 回答"界面文案里哪些码点没被 imgui 的 glyph ranges 覆盖"；
    但"范围里加了字" ≠ "能画出字" —— 字体文件本身没有该字形时，imgui 只会记一个
    空槽，仍然渲染成方块（本工程已在 U+25A1/0x25B6 这类几何符号上踩过：
    旧默认字体 OPPO_B 在 U+2000..U+2BFF 区间只有引号/破折号/• ℃ ™ 之类，
    没有任何几何符号 —— 窗口按钮因此改为用绘制列表画）。

所以修法有两条，必须先分清走哪条：
    · 字体**有**字形 → 在 ui_theme.cpp 的 AddText("…") 里补上这些字；
    · 字体**没有**字形 → 改文案（或像窗口按钮那样改用绘制列表画）。

用法：python tools/check_font_glyphs.py [字体路径] [码点…]
      不带码点时，检查 check_glyph_coverage.py 报出的那批字符。
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_FONT = REPO / "data/font/NotoSansSC-Medium-subset.ttf"

# 默认待查字符：来自 check_glyph_coverage.py 的输出
DEFAULT_CHARS = "Δ←→拓枚耦"


def read_cmap_codepoints(path: Path) -> set[int]:
    """极简 TrueType cmap 解析：取 4(segment mapping)/12(UVS) 子表，返回覆盖码点集。"""
    data = path.read_bytes()
    if len(data) < 12:
        sys.exit(f"{path} 太小，不是字体文件")
    tag = data[:4]
    if tag == b"ttcf":
        sys.exit("这是 TrueType Collection(.ttc)，本脚本只解析单字体 .ttf")
    if tag not in (b"\x00\x01\x00\x00", b"true", b"OTTO"):
        sys.exit(f"{path} 不是可识别的 sfnt 字体（tag={tag!r}）")

    num_tables = struct.unpack(">H", data[4:6])[0]
    cmap_off = None
    for i in range(num_tables):
        off = 12 + i * 16
        rec_tag = data[off:off + 4]
        if rec_tag == b"cmap":
            cmap_off = struct.unpack(">I", data[off + 8:off + 12])[0]
            break
    if cmap_off is None:
        sys.exit("字体里没有 cmap 表")

    num_sub = struct.unpack(">H", data[cmap_off + 2:cmap_off + 4])[0]
    best = None
    for i in range(num_sub):
        rec = cmap_off + 4 + i * 8
        plat, enc = struct.unpack(">HH", data[rec:rec + 4])
        sub_off = cmap_off + struct.unpack(">I", data[rec + 4:rec + 8])[0]
        fmt = struct.unpack(">H", data[sub_off:sub_off + 2])[0]
        # 优先 3/10(或 3/1) + format 4；有 format 12 更好
        score = {12: 3, 4: 2}.get(fmt, 0) + (1 if plat == 3 else 0)
        if score and (best is None or score > best[0]):
            best = (score, fmt, sub_off)
    if best is None:
        sys.exit("cmap 里没有可解析的子表（format 4/12）")

    _, fmt, sub_off = best
    cps: set[int] = set()
    if fmt == 4:
        seg_x2 = struct.unpack(">H", data[sub_off + 6:sub_off + 8])[0]
        seg = seg_x2 // 2
        ends = struct.unpack(">" + "H" * seg, data[sub_off + 14:sub_off + 14 + seg_x2])
        starts_off = sub_off + 16 + seg_x2
        starts = struct.unpack(">" + "H" * seg, data[starts_off:starts_off + seg_x2])
        for s, e in zip(starts, ends):
            if e == 0xFFFF and s == 0xFFFF:
                continue
            cps.update(range(s, e + 1))
    else:   # format 12
        n_groups = struct.unpack(">I", data[sub_off + 12:sub_off + 16])[0]
        for i in range(n_groups):
            g = sub_off + 16 + i * 12
            s, e, _ = struct.unpack(">III", data[g:g + 12])
            cps.update(range(s, e + 1))
    return cps


def main() -> int:
    font = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_FONT
    chars = "".join(sys.argv[2:]) if len(sys.argv) > 2 else DEFAULT_CHARS
    if not font.is_file():
        sys.exit(f"字体不存在: {font}")

    cps = read_cmap_codepoints(font)
    print(f"字体      : {font}")
    print(f"cmap 码点 : {len(cps)} 个")

    have, miss = [], []
    for ch in dict.fromkeys(chars):   # 去重且保序
        (have if ord(ch) in cps else miss).append(ch)

    if have:
        print(f"\n字体**有**字形（→ 在 ui_theme.cpp 的 AddText 里补上即可）: "
              f"{' '.join(have)}")
    if miss:
        print(f"\n字体**没有**字形（→ 补进 ranges 也没用，只会留一个空槽；"
              f"必须改文案或改用绘制列表画）: {' '.join(miss)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
