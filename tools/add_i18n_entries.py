#!/usr/bin/env python3
"""Moleculens · tools/add_i18n_entries.py —— 向 language.cpp 的 kTable 追加词条

用法：python tools/add_i18n_entries.py entries.txt
      entries.txt 每行一条：中文<TAB>English（# 开头的行忽略）

为什么用脚本而不是手改：词条多且含引号/换行/\\0（ImGui 多选项分隔符），手写转义易错；
脚本统一处理转义并**跳过已存在的键**，可反复运行。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TABLE_FILE = REPO / "src/app/i18n/language.cpp"


def cxx_escape(s: str) -> str:
    """按 C++ 字符串字面量规则转义（含 NUL → \\0，供 ImGui 多选项串使用）。"""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\x00":
            out.append("\\0")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return "".join(out)


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit("用法: add_i18n_entries.py entries.txt")
    src = TABLE_FILE.read_text(encoding="utf-8")
    added = skipped = 0
    new_lines = []
    for raw in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        parts = raw.split("\t")
        if len(parts) != 2:
            print(f"!! 跳过格式不对的行（需要 中文<TAB>English）: {raw!r}")
            continue
        zh, en = parts[0], parts[1]
        key = '"' + cxx_escape(zh).replace("\\\\", "\\\\") + '"'
        # 已存在则跳过（以转义后的中文为键判重）
        if f'{{{key},' in src or f'{{ {key},' in src:
            skipped += 1
            continue
        new_lines.append(f'    {{{key}, "{cxx_escape(en)}"}},')
        added += 1

    if added == 0:
        print(f"无新增（跳过 {skipped} 条已存在）")
        return 0

    # 插到 **kTable[]** 的收尾 "};" 之前。
    # ⚠ 不能简单找第一个 "\n};" —— 文件里 struct Entry 的定义也以 "};" 收尾，
    #   那样会把词条插进结构体定义里（本脚本第一版就是这么错的，编译期才暴露）。
    start = src.find("kTable[] = {")
    if start < 0:
        sys.exit("找不到 kTable[] 定义")
    end = src.find("\n};", start)
    if end < 0:
        sys.exit("找不到 kTable 的结尾 };")
    src = src[:end] + "\n\n    // —— 追加词条（tools/add_i18n_entries.py）——\n" + \
        "\n".join(new_lines) + src[end:]
    TABLE_FILE.write_text(src, encoding="utf-8")
    print(f"新增 {added} 条（跳过 {skipped} 条已存在）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
