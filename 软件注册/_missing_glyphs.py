# -*- coding: utf-8 -*-
"""算出「界面文案用到、但 imgui 常用简体字形范围没覆盖」的汉字。

imgui 的 GetGlyphRangesChineseSimplifiedCommon() 把 2500 常用字编码成
「相对 0x4E00 的累加偏移」数组，这里解码该表，再与界面字符串字面量用字求差。
"""
import pathlib
import re

IMGUI_DRAW = pathlib.Path(
    r'F:\vcpkg\buildtrees\imgui\src\.9-docking-449e592d6e.clean\imgui_draw.cpp')
APP_SRC = pathlib.Path(r'F:\MolecuLens\src\app')
OUT = pathlib.Path(r'F:\MolecuLens\软件注册')

CJK = re.compile(u'[一-鿿]')
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def common_set():
    t = IMGUI_DRAW.read_text(encoding='utf-8', errors='ignore')
    i = t.find('accumulative_offsets_from_0x4E00[]')
    # 从 '{' 之后开始取数：声明行里的 "0x4E00" 本身含数字，混进来会让
    # 整个累加序列错位（每个码点都偏移若干位，解出一堆生僻字）。
    i = t.find('{', i)
    j = t.find('};', i)
    nums = [int(x) for x in re.findall(r'-?\d+', t[i:j])]
    out, cp = set(), 0x4E00
    for off in nums:
        cp += off
        out.add(chr(cp))
    return out


def ui_chars():
    chars = set()
    for p in APP_SRC.rglob('*'):
        if p.suffix not in ('.cpp', '.h'):
            continue
        for line in p.read_text(encoding='utf-8', errors='ignore').splitlines():
            code = line.split('//')[0]          # 去行注释
            for lit in LITERAL.findall(code):
                chars.update(CJK.findall(lit))
    return chars


def main():
    common = common_set()
    ui = ui_chars()
    missing = sorted(ui - common)
    OUT.joinpath('_missing.txt').write_text(u''.join(missing), encoding='utf-8')
    OUT.joinpath('_common_sample.txt').write_text(
        u''.join(sorted(common)[:30]), encoding='utf-8')
    report = [
        u'常用表字数: %d' % len(common),
        u'界面字面量汉字数: %d' % len(ui),
        u'常用表外的字数: %d' % len(missing),
        u'抽样(常用表): ' + u''.join(sorted(common)[:30]),
        u'缺失字: ' + u''.join(missing),
    ]
    OUT.joinpath('_glyph_report.txt').write_text(u'\n'.join(report), encoding='utf-8')
    print('report written')


if __name__ == '__main__':
    main()
