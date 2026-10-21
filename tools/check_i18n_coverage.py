#!/usr/bin/env python3
"""Moleculens · tools/check_i18n_coverage.py —— 界面文案的多语言覆盖率自检

背景（见 src/app/i18n/language.h）：
    翻译表以**中文源串本身**为键（`tr("背景色")` → "Background"），未收录时原样
    返回中文。好处是不必另造 key、可增量补齐；代价是"漏翻"完全静默 —— 英文界面里
    突然冒出一句中文，只有肉眼能发现。

本脚本把这件事变成可检查的：
    ① 扫 src/app 下所有字符串字面量里的中文字符；
    ② 解析 i18n/language.cpp 的翻译表，取出已收录的键；
    ③ 报出**未收录**的字面量及其位置。

用法：python tools/check_i18n_coverage.py [--list-missing]
退出码：0 = 全覆盖；1 = 存在未收录文案
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
I18N_TABLE = REPO / "src/app/i18n/language.cpp"
SCAN_DIRS = [REPO / "src/app"]

# 这些文件里的中文是注释/日志/内部诊断，不上屏，不需要翻译
SKIP_FILES = {
    "language.cpp",   # 翻译表自身
}


def has_cjk(s: str) -> bool:
    return any("一" <= ch <= "鿿" or "　" <= ch <= "〿" for ch in s)


# —— 分类：哪些中文串算"界面文案"（需要翻译），哪些不算 ——
# 不计入的三类（都会在统计里单独报数，避免"用排除法把数字做好看"）：
#   · 日志/诊断：以 [xxx] 开头的 spdlog 消息（写窗口的文件日志与日志面板，
#     属诊断而非界面标签；本工程日志一向上中文）
#   · ImGui 内部 ID：形如 "##xxx" / "xxx##yyy"，翻译会改掉窗口 ID →
#     已保存的 Dock 布局与控件状态全部失配
#   · 字体字形清单：ui_theme.cpp 里给 imgui 补字用的那串汉字，不是文案
LOG_PREFIX = re.compile(r"^\s*\[[a-zA-Z]+\]")
IMGUI_ID = re.compile(r"##")
GLYPH_LIST_MAX = 40   # 纯汉字且很长、无空白/标点 → 字形清单


def is_ui_string(s: str) -> bool:
    if LOG_PREFIX.match(s):
        return False
    if IMGUI_ID.search(s):
        return False
    if len(s) >= GLYPH_LIST_MAX and all("一" <= ch <= "鿿" for ch in s):
        return False
    return True


# `builder.AddText("屏帧徽…")` 里那串字是**给字体 atlas 补字形用的数据**，不是界面文案
# —— 它列出的恰恰是"尚未被常用字表覆盖"的字，把它当待翻译文案会永远自相矛盾
# （而且它一旦被 tr() 包住并被收进翻译表，英文模式下 AddText 会收到英文，
#   中文字形根本不会进 atlas）。故整行含 AddText( 的字面量一律不计入。
ADD_TEXT_CALL = re.compile(r"AddText\s*\(")


def table_keys() -> set[str]:
    """解析 language.cpp 里 kTable 的 {"中文", "English"} 键值对。"""
    src = I18N_TABLE.read_text(encoding="utf-8")
    m = re.search(r"kTable\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        sys.exit(f"在 {I18N_TABLE} 里找不到 kTable[]（表结构变了？）")
    keys = set()
    for zh, _en in re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}',
                              m.group(1)):
        keys.add(zh)
    return keys


def scan_literals() -> dict[str, list[str]]:
    """扫字符串字面量里的中文 → {字面量: [文件:行]}。跳过注释行。"""
    found: dict[str, list[str]] = {}
    lit_re = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
    for d in SCAN_DIRS:
        for f in sorted(d.rglob("*.cpp")) + sorted(d.rglob("*.h")):
            if f.name in SKIP_FILES:
                continue
            for lineno, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
                stripped = line.lstrip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue
                if ADD_TEXT_CALL.search(line):
                    continue   # 字形清单，不是界面文案（见 ADD_TEXT_CALL 注释）
                for lit in lit_re.findall(line):
                    if has_cjk(lit):
                        found.setdefault(lit, []).append(
                            f"{f.relative_to(REPO)}:{lineno}")
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list-missing", action="store_true",
                    help="逐条列出未收录文案（默认只报计数与少量样例）")
    args = ap.parse_args()

    keys = table_keys()
    all_literals = scan_literals()
    literals = {k: v for k, v in all_literals.items() if is_ui_string(k)}
    skipped = len(all_literals) - len(literals)

    missing = {k: v for k, v in literals.items() if k not in keys}
    total = len(literals)
    covered = total - len(missing)
    print(f"翻译表条目      : {len(keys)}")
    print(f"界面中文文案    : {total} 条   （另 {skipped} 条为日志/ID/字形清单，不计入）")
    print(f"已收录          : {covered} 条")
    print(f"未收录          : {len(missing)} 条"
          f"{'（英文界面下会显示为中文）' if missing else ''}")

    if missing:
        print()
        items = sorted(missing.items(), key=lambda kv: kv[1][0])
        shown = items if args.list_missing else items[:25]
        for text, locs in shown:
            print(f'  "{text}"  ← {locs[0]}' + (f"  (另 {len(locs)-1} 处)" if len(locs) > 1 else ""))
        if not args.list_missing and len(missing) > len(shown):
            print(f"  … 另有 {len(missing) - len(shown)} 条，用 --list-missing 全部列出")
        print("\n修法：在 src/app/i18n/language.cpp 的 kTable 里补 {中文, English}。")
        return 1

    print("\n多语言覆盖: OK（界面中文文案全部已收录）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
