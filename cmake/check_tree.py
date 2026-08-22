#!/usr/bin/env python3
"""MolecuLens · cmake/check_tree.py —— 目录粒度规则检查

规格来源：主方案 §3.5（任一目录内 .cpp/.h 源文件 ≤ 3 个；.vert/.frag/.cmake/.py
          等非 cpp/h 文件不受此限）。

用法：python check_tree.py [src_dir ...]
退出码：0 通过 / 1 违反
"""
import sys
from pathlib import Path

LIMIT = 3


def main() -> int:
    roots = sys.argv[1:] if len(sys.argv) > 1 else ["src", "tests"]
    bad = 0
    for root in roots:
        base = Path(root)
        if not base.is_dir():
            continue
        for d in sorted(base.rglob("*")):
            if not d.is_dir():
                continue
            files = [f for f in d.iterdir() if f.suffix in (".cpp", ".h")]
            if len(files) > LIMIT:
                print(
                    f"VIOLATION: {d} has {len(files)} cpp/h files (limit {LIMIT}): "
                    + ", ".join(f.name for f in files),
                    file=sys.stderr,
                )
                bad += 1
    if bad:
        print(f"{bad} directory(ies) violate the granularity rule", file=sys.stderr)
        return 1
    print("directory granularity: OK (all dirs <= 3 cpp/h files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
