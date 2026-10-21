#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Markdown → DOCX 转换器（MolecuLens v2 文档交付用）。

用法：
    python tools/md_to_docx.py docs/01-软件架构书.md docs/01-软件架构书.docx

支持：标题 1–4、段落、无序/有序列表（可嵌套一层）、围栏代码块、
管道表格、行内 `代码` 与 **粗体**、图片（存在则嵌入，缺失则留占位）、
引用块、分隔线。图片路径按 Markdown 文件所在目录解析。
"""

from __future__ import annotations

import os
import re
import sys

from docx import Document
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH, WD_BREAK
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Cm, Pt, RGBColor

CJK_FONT = "微软雅黑"
CODE_FONT = "Consolas"
BODY_SIZE = Pt(10.5)

HEADING_SIZES = {1: Pt(20), 2: Pt(16), 3: Pt(13), 4: Pt(11.5)}

INLINE = re.compile(r"(`[^`]+`|\*\*[^*]+\*\*|\*[^*]+\*)")


def set_cjk(run, font=CJK_FONT):
    run.font.name = font
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = OxmlElement("w:rFonts")
        rpr.append(rfonts)
    rfonts.set(qn("w:eastAsia"), font)


def shade(element, hex_fill):
    """给段落或单元格加底色。"""
    pr = element._element.get_or_add_pPr() if hasattr(element, "paragraph_format") else None
    if pr is None:
        return
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), hex_fill)
    pr.append(shd)


def add_inline(paragraph, text, base_bold=False, mono=False):
    """把一行 Markdown 行内标记写进段落。"""
    for chunk in INLINE.split(text):
        if not chunk:
            continue
        if chunk.startswith("`") and chunk.endswith("`") and len(chunk) > 2:
            run = paragraph.add_run(chunk[1:-1])
            set_cjk(run, CODE_FONT)
            run.font.size = Pt(9.5)
            run.font.color.rgb = RGBColor(0xB0, 0x30, 0x60)
            if base_bold:
                run.bold = True
        elif chunk.startswith("**") and chunk.endswith("**") and len(chunk) > 4:
            run = paragraph.add_run(chunk[2:-2])
            set_cjk(run)
            run.bold = True
        elif chunk.startswith("*") and chunk.endswith("*") and len(chunk) > 2:
            run = paragraph.add_run(chunk[1:-1])
            set_cjk(run)
            run.italic = True
        else:
            run = paragraph.add_run(chunk)
            set_cjk(run, CODE_FONT if mono else CJK_FONT)
            if base_bold:
                run.bold = True


def setup_document() -> Document:
    doc = Document()
    section = doc.sections[0]
    section.page_height = Cm(29.7)
    section.page_width = Cm(21.0)
    for attr, value in (("top_margin", 2.2), ("bottom_margin", 2.2),
                        ("left_margin", 2.4), ("right_margin", 2.4)):
        setattr(section, attr, Cm(value))

    normal = doc.styles["Normal"]
    normal.font.name = CJK_FONT
    normal.font.size = BODY_SIZE
    normal.paragraph_format.space_after = Pt(5)
    normal.paragraph_format.line_spacing = 1.35
    rpr = normal.element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = OxmlElement("w:rFonts")
        rpr.append(rfonts)
    rfonts.set(qn("w:eastAsia"), CJK_FONT)

    for level, size in HEADING_SIZES.items():
        style = doc.styles[f"Heading {level}"]
        style.font.name = CJK_FONT
        style.font.size = size
        style.font.bold = True
        style.font.color.rgb = RGBColor(0x1F, 0x29, 0x37)
        style.paragraph_format.space_before = Pt(14 if level <= 2 else 10)
        style.paragraph_format.space_after = Pt(6)
        rpr = style.element.get_or_add_rPr()
        rfonts = rpr.find(qn("w:rFonts"))
        if rfonts is None:
            rfonts = OxmlElement("w:rFonts")
            rpr.append(rfonts)
        rfonts.set(qn("w:eastAsia"), CJK_FONT)
    return doc


def add_toc(doc: Document) -> None:
    p = doc.add_paragraph()
    run = p.add_run()
    fld = OxmlElement("w:fldSimple")
    fld.set(qn("w:instr"), r'TOC \o "1-3" \h \z \u')
    run._element.addnext(fld)
    tip = doc.add_paragraph()
    r = tip.add_run("（目录为 Word 域：打开文档后按 F9 或右键“更新域”即可生成页码。）")
    set_cjk(r)
    r.font.size = Pt(9)
    r.font.color.rgb = RGBColor(0x64, 0x74, 0x8B)
    doc.add_paragraph().add_run().add_break(WD_BREAK.PAGE)


def add_table(doc: Document, rows: list[list[str]]) -> None:
    if not rows:
        return
    cols = max(len(r) for r in rows)
    table = doc.add_table(rows=len(rows), cols=cols)
    table.style = "Table Grid"
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    for ri, row in enumerate(rows):
        for ci in range(cols):
            cell = table.cell(ri, ci)
            cell.text = ""
            paragraph = cell.paragraphs[0]
            paragraph.paragraph_format.space_after = Pt(2)
            add_inline(paragraph, row[ci] if ci < len(row) else "",
                       base_bold=(ri == 0))
            if ri == 0:
                shade(paragraph, "EEF2F7")
    doc.add_paragraph()


def add_code_block(doc: Document, lines: list[str]) -> None:
    for line in lines:
        p = doc.add_paragraph()
        p.paragraph_format.space_after = Pt(0)
        p.paragraph_format.line_spacing = 1.0
        run = p.add_run(line if line else " ")
        set_cjk(run, CODE_FONT)
        run.font.size = Pt(9)
        shade(p, "F4F6FA")
    doc.add_paragraph()


def add_image(doc: Document, base_dir: str, alt: str, target: str) -> None:
    path = target.strip()
    if not os.path.isabs(path):
        path = os.path.normpath(os.path.join(base_dir, path))
    if os.path.isfile(path):
        try:
            doc.add_picture(path, width=Cm(15.5))
            doc.paragraphs[-1].alignment = WD_ALIGN_PARAGRAPH.CENTER
            if alt:
                cap = doc.add_paragraph()
                cap.alignment = WD_ALIGN_PARAGRAPH.CENTER
                run = cap.add_run(alt)
                set_cjk(run)
                run.font.size = Pt(9)
                run.font.color.rgb = RGBColor(0x64, 0x74, 0x8B)
            return
        except Exception as exc:  # pragma: no cover - 图像损坏时退化为占位
            print(f"[warn] 图片嵌入失败 {path}: {exc}")
    p = doc.add_paragraph()
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    run = p.add_run(f"［插图占位：{alt or '示意图'}（{os.path.basename(path)}）］")
    set_cjk(run)
    run.font.color.rgb = RGBColor(0x94, 0xA3, 0xB8)


def convert(md_path: str, docx_path: str) -> None:
    with open(md_path, "r", encoding="utf-8") as handle:
        lines = handle.read().splitlines()

    base_dir = os.path.dirname(os.path.abspath(md_path))
    doc = setup_document()
    add_toc(doc)

    i = 0
    n = len(lines)
    in_code = False
    code_buf: list[str] = []
    table_buf: list[list[str]] = []

    def flush_table():
        nonlocal table_buf
        if table_buf:
            add_table(doc, table_buf)
            table_buf = []

    while i < n:
        raw = lines[i]
        line = raw.rstrip()
        stripped = line.strip()

        if in_code:
            if stripped.startswith("```"):
                in_code = False
                add_code_block(doc, code_buf)
                code_buf = []
            else:
                code_buf.append(raw)
            i += 1
            continue

        if stripped.startswith("```"):
            flush_table()
            in_code = True
            i += 1
            continue

        # 表格：连续的 | 行，第二行是 |---|---|
        if stripped.startswith("|") and stripped.endswith("|"):
            cells = [c.strip() for c in stripped.strip("|").split("|")]
            if all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
                i += 1
                continue
            table_buf.append(cells)
            i += 1
            continue
        flush_table()

        if not stripped:
            i += 1
            continue

        if re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", stripped):
            doc.add_paragraph().add_run().add_break(WD_BREAK.LINE)
            i += 1
            continue

        image = re.match(r"!\[([^\]]*)\]\(([^)]+)\)", stripped)
        if image:
            add_image(doc, base_dir, image.group(1), image.group(2))
            i += 1
            continue

        heading = re.match(r"^(#{1,6})\s+(.*)$", stripped)
        if heading:
            level = min(len(heading.group(1)), 4)
            text = re.sub(r"\s*#+\s*$", "", heading.group(2)).strip()
            paragraph = doc.add_heading(level=level)
            add_inline(paragraph, text)
            for run in paragraph.runs:
                set_cjk(run)
            i += 1
            continue

        quote = re.match(r"^>\s?(.*)$", stripped)
        if quote:
            p = doc.add_paragraph()
            p.paragraph_format.left_indent = Cm(0.8)
            add_inline(p, quote.group(1))
            for run in p.runs:
                run.italic = True
                run.font.color.rgb = RGBColor(0x47, 0x55, 0x69)
            i += 1
            continue

        bullet = re.match(r"^(\s*)[-*+]\s+(.*)$", raw)
        if bullet:
            indent = len(bullet.group(1))
            style = "List Bullet" if indent < 2 else "List Bullet 2"
            p = doc.add_paragraph(style=style)
            p.paragraph_format.space_after = Pt(3)
            add_inline(p, bullet.group(2))
            i += 1
            continue

        ordered = re.match(r"^(\s*)\d+[.)]\s+(.*)$", raw)
        if ordered:
            indent = len(ordered.group(1))
            style = "List Number" if indent < 2 else "List Number 2"
            p = doc.add_paragraph(style=style)
            p.paragraph_format.space_after = Pt(3)
            add_inline(p, ordered.group(2))
            i += 1
            continue

        # 普通段落：把紧随其后的非空行并进来（Markdown 软换行）
        buf = [stripped]
        i += 1
        while i < n:
            nxt = lines[i].strip()
            if (not nxt or nxt.startswith(("#", "|", "```", ">", "-", "*", "!", "  "))
                    or re.match(r"^\d+[.)]\s", nxt)):
                break
            buf.append(nxt)
            i += 1
        p = doc.add_paragraph()
        add_inline(p, "".join(buf))

    flush_table()
    if in_code and code_buf:
        add_code_block(doc, code_buf)

    doc.save(docx_path)
    print(f"[ok] {md_path} -> {docx_path}")


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 1
    convert(argv[1], argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
