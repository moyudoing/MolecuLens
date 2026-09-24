# -*- coding: utf-8 -*-
"""生成软著「源程序」文档：每页 50 行，页眉带软件名称与版本号。

收录顺序按**阅读依赖**排（入口 → 头文件 → 实现 → 操作前端），
不是文件名字典序 —— 这样前 25 页正好是核心数值代码。
"""
from pathlib import Path

from docx import Document
from docx.enum.text import WD_BREAK, WD_LINE_SPACING
from docx.oxml.ns import qn
from docx.shared import Pt, Emu

CPP = Path(r'H:\2026数学建模\cpp')
OUTDIR = CPP / '参考资料'

SOFT_FULL = '药材烘干过程数值模拟软件'
SOFT_SHORT = '烘干模拟'
VERSION = 'V1.0'
HEADER = f'{SOFT_FULL}{VERSION}'

LINES_PER_PAGE = 50
FRONT_PAGES = 25
BACK_PAGES = 25

# 收录顺序：入口 → 头文件（按依赖）→ 实现 → 操作前端 → 构建脚本
ORDER = [
    'src/main.cpp',
    'include/config.hpp', 'include/util.hpp', 'include/params.hpp',
    'include/grid.hpp', 'include/props.hpp', 'include/psychro.hpp',
    'include/transition.hpp', 'include/interp.hpp', 'include/brent.hpp',
    'include/solver.hpp', 'include/moving.hpp', 'include/budget.hpp',
    'include/run.hpp',
    'src/params.cpp', 'src/grid.cpp', 'src/props.cpp', 'src/psychro.cpp',
    'src/transition.cpp', 'src/interp.cpp', 'src/solver.cpp',
    'src/moving.cpp', 'src/budget.cpp', 'src/run.cpp', 'src/selfcheck.cpp',
    'python/__init__.py', 'python/defaults.py', 'python/specs.py',
    'python/prep_tables.py', 'python/runner.py', 'python/convert.py',
    'python/postproc.py', 'python/gridmap.py', 'python/verify.py',
    'python/check_format.py', 'python/cli.py', 'python/gui.py',
    'python/gui_plots.py',
    'build.ps1', 'CMakeLists.txt',
]


def collect():
    out = []
    for rel in ORDER:
        p = CPP / rel
        if not p.exists():
            print(f'  ! 缺文件 {rel}')
            continue
        out.append(f'// ======== 文件：{rel} ========')
        for ln in p.read_text(encoding='utf-8').splitlines():
            out.append(ln.rstrip())
        out.append('')
    return out


def style_run(run):
    run.font.name = 'Consolas'
    run.font.size = Pt(9)
    # 中文字形走宋体（Consolas 无中日韩字形，不设的话 Word 会回退得很难看）
    run._element.rPr.rFonts.set(qn('w:eastAsia'), '宋体')


def add_page(doc, lines, first=False):
    if not first:
        p = doc.add_paragraph()
        p.paragraph_format.line_spacing_rule = WD_LINE_SPACING.EXACTLY
        p.paragraph_format.line_spacing = Pt(13.5)
        p.add_run().add_break(WD_BREAK.PAGE)
    for s in lines:
        p = doc.add_paragraph()
        pf = p.paragraph_format
        pf.line_spacing_rule = WD_LINE_SPACING.EXACTLY
        pf.line_spacing = Pt(13.5)
        pf.space_before = Pt(0)
        pf.space_after = Pt(0)
        style_run(p.add_run(s if s else ' '))


# 排版宽度预算。
# 正文区宽 = A4 宽 (595.28 pt) − 左右各 1.25 in (90 pt) = 415.28 pt。
# 9 pt 的 Consolas 每个半角字符约 4.95 pt，宋体全角字符 9 pt ⇒ 全角按 2 个
# 半角计。取 80 作容量留一点余量，避免行尾恰好卡在边界上被折行。
CAP_HALFWIDTH = 80


def disp_width(s):
    """按半角宽度算一行的显示宽度（中文/全角字符记 2）。"""
    return sum(1 if ord(ch) < 128 else 2 for ch in s)


def pack(lines, capacity=CAP_HALFWIDTH):
    """把源码行按**渲染行数**分页：每页恰好 LINES_PER_PAGE 个渲染行。

    ⚠ 不能按"每页塞 50 个源文件行"来分 —— 长中文注释会折行，
      实际渲染行数超过 50，页数会从 50 涨到 62（踩过）。
    """
    pages, cur, used = [], [], 0
    for s in lines:
        w = max(1, -(-disp_width(s or ' ') // capacity))
        if used + w > LINES_PER_PAGE and cur:
            pages.append(cur)
            cur, used = [], 0
        cur.append(s)
        used += w
    if cur:
        pages.append(cur)
    return pages


def main():
    lines = collect()
    pages = pack(lines)
    total = len(lines)
    print(f'源程序共 {total} 行，按渲染行分页后 {len(pages)} 页')

    front = pages[:FRONT_PAGES]
    back = pages[-BACK_PAGES:] if len(pages) > FRONT_PAGES + BACK_PAGES else []

    doc = Document()
    sec = doc.sections[0]
    sec.page_width, sec.page_height = Emu(7560310), Emu(10692130)   # A4
    sec.top_margin = sec.bottom_margin = Emu(914400)
    sec.left_margin = sec.right_margin = Emu(1143000)
    sec.header_distance = Emu(457200)

    for s in doc.sections:
        hp = s.header.paragraphs[0]
        hp.text = ''
        r = hp.add_run(HEADER)
        r.font.size = Pt(9)
        r.font.name = '宋体'
        r._element.rPr.rFonts.set(qn('w:eastAsia'), '宋体')

    first = True
    for chunk in front:
        add_page(doc, chunk, first=first)
        first = False
    for chunk in back:
        add_page(doc, chunk, first=False)

    out = OUTDIR / f'{SOFT_SHORT}_源程序.docx'
    doc.save(out)
    print(f'已生成 {out}')
    nfront = sum(len(c) for c in front)
    nback = sum(len(c) for c in back)
    print(f'  前 {len(front)} 页：前 {nfront} 个源文件行')
    if back:
        print(f'  后 {len(back)} 页：末 {nback} 个源文件行（自第 {total-nback+1} 行起）')
    else:
        print('  源程序不足 50 页，已全部收录。')


if __name__ == '__main__':
    main()
