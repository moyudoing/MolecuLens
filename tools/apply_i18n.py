#!/usr/bin/env python3
"""Moleculens · tools/apply_i18n.py —— 把界面中文文案包上 tr(...)

配合 src/app/i18n/language.h 的"以中文源串为键"设计：本脚本把 UI 代码里的中文
字面量替换成 `tr("中文")`，翻译本身在 language.cpp 的表里补。

**幂等**：已包过 tr(...) 的不会再包；可反复运行（新增文案后再跑一次即可）。

自动跳过三类（包了会出问题或没有意义）：
  · 日志/诊断行（含 spdlog / printf / fprintf）—— 写日志用，非界面标签
  · 注释行
  · i18n 自身与 vendored 的 imgui 后端

特例处理：**相邻字面量拼接**（如 ImGui 多选项 `"8-bit\\0" "16-bit (PNG)\\0"`）必须
整体包一次 `tr("8-bit\\0" "16-bit (PNG)\\0")` —— 逐段包会变成 `tr(a) tr(b)`，
那不是合法 C++。本脚本按"最大相邻串"处理。

用法：python tools/apply_i18n.py [--check]
       --check 只报告将改动的行数，不写文件
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCAN_DIRS = [REPO / "src/app"]
SKIP_PARTS = ("i18n", "imgui\\backends", "imgui/backends")
# 这些文件里的中文**不能**用本脚本包 tr：
#   · dock_layout.cpp —— 面板窗口 ID（参与了 Dock 布局持久化，翻译会让布局失配）
#   · *.h —— 面板标题（窗口名），已在 BaseToolView::windowLabel 里统一处理
#     （显示名走 tr、ID 用 ### 固定），脚本再包一层会变成 tr() 套 tr()
SKIP_FILES = ("dock_layout.cpp",)

# 这些行不包：写日志/诊断输出，不是界面标签
LOG_MARKERS = ("spdlog::", "printf(", "fprintf(", "std::printf(")

LIT = r'"(?:[^"\\\n]|\\.)*"'
# 最大相邻字面量串（含中间空白）
ADJACENT = re.compile(LIT + r'(?:\s*' + LIT + r')*')
CJK = re.compile(r"[一-鿿　-〿]")


def has_cjk(s: str) -> bool:
    return bool(CJK.search(s))


def wrap_line(line: str) -> tuple[str, int]:
    """把一行里的中文相邻字面量串包上 tr(...)。返回 (新行, 替换次数)。"""
    if line.lstrip().startswith("//") or line.lstrip().startswith("*"):
        return line, 0
    if any(m in line for m in LOG_MARKERS):
        return line, 0
    if "#include" in line:
        return line, 0

    out = []
    pos = 0
    count = 0
    for m in ADJACENT.finditer(line):
        if not has_cjk(m.group(0)):
            continue
        # 已经是 tr(...) 的跳过（检查前一个非空白字符序列）
        prefix = line[:m.start()].rstrip()
        if prefix.endswith("tr("):
            continue
        out.append(line[pos:m.start()])
        out.append("tr(" + m.group(0) + ")")
        pos = m.end()
        count += 1
    out.append(line[pos:])
    return "".join(out), count


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    changed_files = 0
    changed_literals = 0
    for d in SCAN_DIRS:
        for f in sorted(d.rglob("*.cpp")) + sorted(d.rglob("*.h")):
            rel = str(f.relative_to(REPO))
            if any(p in rel.replace("/", "\\") for p in SKIP_PARTS):
                continue
            if f.name in SKIP_FILES or f.suffix == ".h":
                continue
            src = f.read_text(encoding="utf-8")
            lines = src.splitlines(keepends=True)
            total = 0
            new_lines = []
            for ln in lines:
                nl, n = wrap_line(ln)
                new_lines.append(nl)
                total += n
            if total:
                changed_files += 1
                changed_literals += total
                print(f"  {rel}: {total} 处")
                if not args.check:
                    # newline="" 原样写回：splitlines(keepends=True) 保留了原有行尾
                    # （CRLF 不会被转成 LF，反之亦然）—— 避免整文件行尾翻转
                    with f.open("w", encoding="utf-8", newline="") as fh:
                        fh.write("".join(new_lines))

    verb = "将改动" if args.check else "已改动"
    print(f"\n{verb} {changed_files} 个文件 / {changed_literals} 处字面量")
    return 0


if __name__ == "__main__":
    sys.exit(main())
