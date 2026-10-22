#!/usr/bin/env python3
"""MolecuLens · cmake/check_tree.py —— 目录粒度规则检查

规格来源：主方案 §3.5（任一目录内 .cpp/.h 源文件 ≤ 3 个；.vert/.frag/.cmake/.py
          等非 cpp/h 文件不受此限）。

例外目录（在文件内显式登记，避免"为了过检查而搬文件"）：
  * src/app/imgui/backends —— vendored Dear ImGui 的 GLFW / Vulkan 后端，
    文件名由上游固定，不能拆分或改名；
  * tests/entity/trajectory —— 每种轨迹格式（DCD / XTC / TRR）一个测试文件；
  * tests/vulkan —— 每条 Vulkan 路径（线框 / 网格 / 离屏 / 单原子）一个冒烟测试。

用法：python check_tree.py [src_dir ...]
退出码：0 通过 / 1 违反
"""
import sys
from pathlib import Path

LIMIT = 3

EXEMPT_DIRS = {
    "src/app/imgui/backends": "vendored Dear ImGui backends (upstream filenames)",
    "tests/entity/trajectory": "one test file per trajectory format",
    "tests/vulkan": "one smoke test per Vulkan path",
}


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
            rel = d.as_posix()
            if rel in EXEMPT_DIRS:
                print(f"EXEMPT: {rel} ({EXEMPT_DIRS[rel]})")
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
    print("directory granularity: OK (all dirs <= 3 cpp/h files, exemptions applied)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
