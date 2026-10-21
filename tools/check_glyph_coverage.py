#!/usr/bin/env python3
"""Moleculens · tools/check_glyph_coverage.py —— UI 文案字形覆盖自检

背景（见 app/view/theme/ui_theme.cpp 的 loadFont 注释）：
    字体只加载 "默认范围 + 中文常用字表 + 显式补字" 三类码点。界面文案里若出现
    三者都未覆盖的汉字，imgui 拿不到字形 → 渲染成方块（"豆腐块"），且**不会报错**。
    历史上已经踩过：常用字表里没有"渲"，"渲染设置"面板标题显示成方块。

本脚本把这件事从"上线后肉眼发现"变成"提交前可检查"：
    ① 从 imgui 源码里抽出 GetGlyphRangesDefault / GetGlyphRangesChineseSimplifiedCommon
       两张实际使用的范围表；
    ② 扫 src/app 下所有字符串字面量里的非 ASCII 字符；
    ③ 减去 ui_theme.cpp 里 AddChar 显式补的码点；
    ④ 报出仍未覆盖的字符及其出现位置。

用法：python tools/check_glyph_coverage.py [--imgui <imgui_draw.cpp 路径>]
退出码：0 = 全覆盖；1 = 存在未覆盖字符
"""
from __future__ import annotations   # 本机 Python 3.9：注解里的 X | None 需延迟求值

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
UI_THEME = REPO / "src/app/view/theme/ui_theme.cpp"

# 只看界面文案所在的目录（engine/entity 的字符串是日志/格式名，不上屏）
SCAN_DIRS = [REPO / "src/app"]


def find_imgui_draw(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        sys.exit(f"指定的 imgui_draw.cpp 不存在: {p}")
    # vcpkg 的 buildtree 里保留着实际编译用的源码
    roots = [Path(r"F:/vcpkg/buildtrees/imgui/src"), Path(r"F:/vcpkg/buildtrees/imgui")]
    for root in roots:
        if root.is_dir():
            hits = sorted(root.rglob("imgui_draw.cpp"))
            if hits:
                return hits[0]
    sys.exit("未找到 imgui_draw.cpp —— 请用 --imgui 显式指定")


def func_body(src: str, func_name: str) -> str:
    m = re.search(r"GetGlyphRanges" + func_name + r"\s*\(\s*\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        sys.exit(f"在 imgui 源码里找不到 GetGlyphRanges{func_name}()")
    return m.group(1)


def extract_ranges(src: str, func_name: str) -> list[tuple[int, int]]:
    """函数体内显式写出的 `0xAAAA, 0xBBBB,` 范围对。"""
    pairs = re.findall(r"(0x[0-9A-Fa-f]{4})\s*,\s*(0x[0-9A-Fa-f]{4})", func_body(src, func_name))
    return [(int(a, 16), int(b, 16)) for a, b in pairs]


def extract_accumulative(src: str, func_name: str, var_name: str, base: int) -> set[int]:
    """抽出 `short <var_name>[] = { … };` 里以**累积偏移**编码的码点集合。

    imgui 的中文常用字表不是显式范围，而是"相对 0x4E00 的累积偏移"数组
    （UnpackAccumulativeOffsetsIntoRanges 逐项累加还原）：
        base_codepoint += offsets[n];  码点 = base + 该前缀和
    2500 个字按此编码，若按显式范围去解析只会读到表头表尾那几对 —— 那会让
    绝大多数常用汉字被误判成"未覆盖"（本脚本第一版就踩了这个坑）。
    """
    m = re.search(
        r"(?:const\s+short|short)\s+" + var_name + r"\s*\[\s*\]\s*=\s*\{(.*?)\};",
        func_body(src, func_name),
        re.S,
    )
    if not m:
        sys.exit(f"找不到 {var_name}[]（imgui 表结构可能变了）")
    offsets = [int(n) for n in re.findall(r"-?\d+", m.group(1))]
    out: set[int] = set()
    cp = base
    for off in offsets:
        cp += off
        out.add(cp)
    return out


def covered(cp: int, ranges: list[tuple[int, int]], singles: set[int]) -> bool:
    if cp in singles:
        return True
    return any(lo <= cp <= hi for lo, hi in ranges)


def explicit_addchars(src: str) -> set[int]:
    """ui_theme.cpp 里 AddChar(0x....) 显式补的码点"""
    return {int(h, 16) for h in re.findall(r"AddChar\(\s*(0x[0-9A-Fa-f]+)\s*\)", src)}


def explicit_addtext(src: str) -> set[int]:
    """ui_theme.cpp 里 AddText("……") 显式补的字符"""
    out: set[int] = set()
    for lit in re.findall(r'AddText\(\s*"((?:[^"\\]|\\.)*)"\s*\)', src):
        out.update(ord(c) for c in lit if ord(c) > 0x7F)
    return out


def scan_ui_literals() -> dict[int, list[str]]:
    """扫源码里字符串字面量的非 ASCII 字符 → {码点: [文件:行]}"""
    found: dict[int, list[str]] = {}
    # 粗略匹配 "..." 字面量（足够覆盖本工程的 UI 文案写法）
    lit_re = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
    for d in SCAN_DIRS:
        for f in sorted(d.rglob("*.cpp")) + sorted(d.rglob("*.h")):
            for lineno, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
                stripped = line.lstrip()
                if stripped.startswith("//"):
                    continue
                for lit in lit_re.findall(line):
                    for ch in lit:
                        if ord(ch) > 0x7F:
                            found.setdefault(ord(ch), []).append(
                                f"{f.relative_to(REPO)}:{lineno}"
                            )
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--imgui", default=None)
    args = ap.parse_args()

    imgui_path = find_imgui_draw(args.imgui)
    imgui_src = imgui_path.read_text(encoding="utf-8", errors="replace")
    theme_src = UI_THEME.read_text(encoding="utf-8")

    # ui_theme.cpp 实际使用的范围：Default + ChineseSimplifiedCommon
    ranges = extract_ranges(imgui_src, "Default")
    ranges += extract_ranges(imgui_src, "ChineseSimplifiedCommon")
    # 中文常用字表（累积偏移编码，2500 字）
    singles = extract_accumulative(
        imgui_src, "ChineseSimplifiedCommon", "accumulative_offsets_from_0x4E00", 0x4E00
    )
    addchars = explicit_addchars(theme_src)
    addtext = explicit_addtext(theme_src)
    singles |= addchars
    singles |= addtext

    print(f"imgui 源码      : {imgui_path}")
    print(f"显式范围段数    : {len(ranges)}（Default + ChineseSimplifiedCommon 的 base_ranges）")
    print(f"常用字表单字    : {len(singles) - len(addchars) - len(addtext)} 个")
    print(f"AddChar 显式补字: {len(addchars)} 个")
    print(f"AddText 显式补字: {len(addtext)} 个 -> {''.join(sorted(chr(c) for c in addtext))}")

    literals = scan_ui_literals()
    cjk = {cp: locs for cp, locs in literals.items() if 0x2E80 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF}
    print(f"界面文案非 ASCII: {len(literals)} 个码点（其中汉字/符号块 {len(cjk)} 个）")

    missing = {}
    for cp, locs in sorted(literals.items()):
        if covered(cp, ranges, singles):
            continue
        missing[cp] = locs

    if not missing:
        print("\n字形覆盖: OK（界面文案全部有字形）")
        return 0

    print(f"\n字形覆盖: {len(missing)} 个码点**未覆盖** —— 会渲染成方块：")
    for cp, locs in missing.items():
        try:
            name = f"U+{cp:04X} {chr(cp)!r}"
        except ValueError:
            name = f"U+{cp:04X}"
        print(f"  {name}")
        for loc in locs[:4]:
            print(f"      {loc}")
        if len(locs) > 4:
            print(f"      … 另 {len(locs) - 4} 处")
    print("\n修法：在 ui_theme.cpp 的 builder.AddText(\"...\") 里补上这些字。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
