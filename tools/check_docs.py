#!/usr/bin/env python3
"""MolecuLens · tools/check_docs.py —— 文档质量检查：相对链接 + 禁用术语

背景：
    文档（`docs/` 与仓库根目录的 *.md）会发布到 GitHub Pages。相对链接写错，本地
    看不出来，打开网页才 404；另外本项目已停用某个第三方分子可视化软件的名字，
    文档正文里不允许再出现该三字母缩写（见下方 TERM_RE）。

本脚本做两件事：
    ① 逐个 *.md 检查**相对**的 Markdown 链接 / 图片目标是否存在。除了
       `[文字](目标)` / `![图](目标)` 与引用式定义 `[id]: 目标`，也检查
       HTML 的 `<img src>`、`<a href>`（README 的画廊就是 HTML 表格）。
    ② 逐个 *.md 扫正文，出现该禁用缩写（词边界、大小写不敏感）即报错。

刻意不检查：
    * 带协议的 URL：http(s)、mailto、data 等；协议相对 `//host/...`；
    * 纯页内锚点 `#8-已知限制`（同一文档内的跳转由 Markdown 渲染器负责）；
    * 带协议的外链目标里 `#anchor` / `?query` 不去解析；
    * 围栏代码块（``` / ~~~）里的内容、行内代码（`...`）里的内容。

相对链接的解析规则：
    以所在 .md 的目录为基准；`/foo` 视为仓库根目录（与 GitHub 一致）；
    `%20` 这类百分号编码先解码再判断；`目标#锚点`、`目标?查询` 只校验目标部分。

用法：
    python tools/check_docs.py            # 检查，只打印问题与汇总
    python tools/check_docs.py --list     # 额外列出扫描到的文件
    python tools/check_docs.py --root docs    # 只扫某个子目录
退出码：0 通过 / 1 有问题（链接断 or 出现禁用词）

纯标准库，Python 3.9+；Windows 与 Linux 下行为一致。
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from urllib.parse import unquote

REPO = Path(__file__).resolve().parent.parent

# —— 不扫的目录：构建产物、依赖、第三方参考源码、编辑器/运行时状态 ——
SKIP_DIR_NAMES = {
    ".git", ".hg", ".svn", ".venv", "venv", "__pycache__", "node_modules",
    "vcpkg_installed", "other", "reference", "out", "logs", ".idea", ".vscode",
    ".monitor", ".tmp", ".dsh-progress", "_aclrepair", "_testtmp", "site",
    "软件注册",
}
# build / build_dsh / build_rel、cmake-build-debug、_test、_testtmp、_esp ...
SKIP_DIR_PREFIXES = ("build", "cmake-build", "_test", "_esp")

# 项目级禁用术语：某个第三方分子可视化软件的三字母缩写。为避免本脚本自身被同一条
# 规则扫中，这里逐字母写成字符类（等价于大小写不敏感的词边界匹配）。
# 注意下划线算词字符，所以 `figs_v2/xxx_glass.png` 这类**文件名**不会被误报。
TERM_RE = re.compile(r"\b[Vv][Mm][Dd]\b")

# 围栏代码块：``` 或 ~~~，开合标记长度需匹配
FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
# 行内代码：`code` / ``code``
INLINE_CODE_RE = re.compile(r"(`+)(?:.+?)\1")
# 行内链接与图片：以 `](目标)` 提取，兼容 [![badge](img.png)](链接) 这种嵌套
LINK_RE = re.compile(
    r"\]\(\s*"
    r"(<[^>]*>|[^\s()<>]+(?:\([^\s()]*\)[^\s()<>]*)*)"
    r"(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^()]*\)))?"
    r"\s*\)"
)
# 引用式定义：[id]: 目标
REF_DEF_RE = re.compile(r"^ {0,3}\[([^\]]+)\]:\s*(\S+)")
# HTML 里的 <img src> / <a href> / <source src>
HTML_ATTR_RE = re.compile(
    r"""<[a-zA-Z][^>]*?\b(?:src|href)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))""",
    re.IGNORECASE,
)
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.\-]*:")

MAX_REPORT = 200  # 每类问题最多打印这么多条，避免刷屏


def is_skipped_dir(name: str) -> bool:
    return name in SKIP_DIR_NAMES or name.startswith(SKIP_DIR_PREFIXES)


def iter_markdown_files(root: Path):
    """按目录顺序产出 root 下的 *.md，跳过构建/依赖/参考目录。"""
    for dirpath, dirnames, filenames in os.walk(str(root)):
        dirnames[:] = sorted(d for d in dirnames if not is_skipped_dir(d))
        here = Path(dirpath)
        for name in sorted(filenames):
            if name.lower().endswith(".md"):
                yield here / name


def read_text(path: Path) -> str:
    """读 UTF-8（容忍 BOM 与零星非法字节），永不为编码问题中断检查。"""
    try:
        return path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as exc:  # 权限 / 占用等：当成空文件，后面照样统计
        print(f"WARN: 无法读取 {path}: {exc}", file=sys.stderr)
        return ""


def rel(path: Path) -> str:
    try:
        return path.relative_to(REPO).as_posix()
    except ValueError:
        return path.as_posix()


def check_target(md_file: Path, raw: str) -> tuple[bool, str]:
    """判断一个链接目标是否需要校验、是否存在。

    返回 (ok, 规范化后的目标)；ok 为 False 表示断链，目标为展示用的原始串。
    """
    target = raw.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1].strip()
    if not target:
        return True, ""            # 空链接：交给别的工具管，不在这里报
    if SCHEME_RE.match(target) or target.startswith("//"):
        return True, ""            # 外链：不校验
    # 去掉锚点与查询串后再看
    target = target.split("#", 1)[0].split("?", 1)[0]
    if not target:
        return True, ""            # 纯页内锚点
    decoded = unquote(target)
    if decoded.startswith("/"):
        candidate = REPO / decoded.lstrip("/")
    else:
        candidate = md_file.parent / decoded.replace("\\", "/")
    return candidate.exists(), decoded


def scan_file(md_file: Path, links: list, terms: list) -> int:
    """扫一个文件：返回检查过的相对链接数，断链/禁用词追加到 links/terms。"""
    text = read_text(md_file)
    checked = 0
    fence: tuple[str, int] | None = None   # (标记字符, 长度)
    for lineno, line in enumerate(text.splitlines(), start=1):
        # ① 禁用术语：围栏代码块里也扫，这是项目级清理，不留死角
        for m in TERM_RE.finditer(line):
            terms.append((md_file, lineno, m.group(0)))

        # ② 围栏代码块状态机
        fence_match = FENCE_RE.match(line)
        if fence is not None:
            if fence_match:
                marker = fence_match.group(1)
                if marker[0] == fence[0] and len(marker) >= fence[1]:
                    fence = None
            continue
        if fence_match:
            marker = fence_match.group(1)
            fence = (marker[0], len(marker))
            continue

        # ③ 行内代码里的东西不是链接
        body = INLINE_CODE_RE.sub(" ", line)

        candidates: list[str] = []
        ref = REF_DEF_RE.match(body)
        if ref:
            candidates.append(ref.group(2))
        candidates.extend(m.group(1) for m in LINK_RE.finditer(body))
        if "<" in body:
            for m in HTML_ATTR_RE.finditer(body):
                candidates.append(next(g for g in m.groups() if g is not None))

        for raw in candidates:
            ok, shown = check_target(md_file, raw)
            if not shown:
                continue
            checked += 1
            if not ok:
                links.append((md_file, lineno, shown))
    return checked


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="检查 Markdown 相对链接与禁用术语（MolecuLens 文档门禁）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--list", action="store_true", help="列出扫描到的 *.md")
    parser.add_argument("--root", default=None, help="只扫这个子目录（默认整个仓库）")
    parser.add_argument("--max-report", type=int, default=MAX_REPORT,
                        help=f"每类问题最多打印条数（默认 {MAX_REPORT}）")
    args = parser.parse_args(argv)

    root = (REPO / args.root).resolve() if args.root else REPO
    if not root.is_dir():
        print(f"ERROR: 目录不存在: {root}", file=sys.stderr)
        return 2

    files = list(iter_markdown_files(root))
    links: list[tuple[Path, int, str]] = []
    terms: list[tuple[Path, int, str]] = []
    checked_links = 0

    for md_file in files:
        checked_links += scan_file(md_file, links, terms)

    if args.list:
        print(f"扫描到 {len(files)} 个 Markdown 文件：")
        for i, md_file in enumerate(files, start=1):
            print(f"  [{i:03d}] {rel(md_file)}")
        print()

    if links:
        print("链接目标不存在：")
        for md_file, lineno, target in links[: args.max_report]:
            print(f"  {rel(md_file)}:{lineno}  ->  {target}")
        if len(links) > args.max_report:
            print(f"  …… 其余 {len(links) - args.max_report} 条已省略")
        print()

    if terms:
        print("出现禁用术语（第三方软件缩写）：")
        for md_file, lineno, word in terms[: args.max_report]:
            print(f"  {rel(md_file)}:{lineno}  {word}")
        if len(terms) > args.max_report:
            print(f"  …… 其余 {len(terms) - args.max_report} 条已省略")
        print()

    bad = len(links) + len(terms)
    status = "失败" if bad else "通过"
    print(
        f"汇总：{len(files)} 个文件 / {checked_links} 个相对链接 / "
        f"{bad} 处问题（断链 {len(links)}、禁用词 {len(terms)}）-> {status}"
    )
    return 1 if bad else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):  # Windows 控制台按 UTF-8 输出中文
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    sys.exit(main())
