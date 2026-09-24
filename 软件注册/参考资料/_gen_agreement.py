# -*- coding: utf-8 -*-
"""生成《多人软件合作开发协议》。

参与人姓名与身份证号属于个人信息，无法代为填写 —— 一律留成
【　】占位，并在文末列出需要补填的清单。
"""
from pathlib import Path

from docx import Document
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from docx.shared import Pt, Emu

OUTDIR = Path(r'H:\2026数学建模\cpp\软著注册')
SOFT_FULL = '药材烘干过程数值模拟软件'
SOFT_SHORT = '烘干模拟'
VERSION = 'V1.0'

doc = Document()
s = doc.sections[0]
s.page_width, s.page_height = Emu(7560310), Emu(10692130)
s.top_margin = s.bottom_margin = Emu(914400)
s.left_margin = s.right_margin = Emu(1143000)

st = doc.styles['Normal']
st.font.name = '宋体'
st.font.size = Pt(12)
st._element.rPr.rFonts.set(qn('w:eastAsia'), '宋体')
st.paragraph_format.line_spacing = 1.6
st.paragraph_format.space_after = Pt(4)


def H(text, size=16, before=0, after=14):
    p = doc.add_paragraph()
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    p.paragraph_format.space_before = Pt(before)
    p.paragraph_format.space_after = Pt(after)
    r = p.add_run(text)
    r.font.name = '黑体'
    r.font.size = Pt(size)
    r._element.rPr.rFonts.set(qn('w:eastAsia'), '黑体')
    return p


def P(text, indent=True, align=None):
    p = doc.add_paragraph(text)
    if indent:
        p.paragraph_format.first_line_indent = Pt(24)
    if align:
        p.alignment = align
    return p


H(SOFT_FULL)
H('多人合作开发协议', size=15)
P('')

P('甲方（第一著作权人）：【　　　　　　　】', indent=False)
P('乙方（合作开发人）：【　　　　　　　】', indent=False)
P('丙方（合作开发人）：【　　　　　　　】', indent=False)
P('')
P('上述各方经协商一致，就合作开发本协议所述软件事宜达成如下约定。')

P('第一条　开发标的', indent=False)
P(f'本协议所称软件，指“{SOFT_FULL}”（简称“{SOFT_SHORT}”，'
  f'版本号 {VERSION}），以下简称“本软件”。本软件用于对药材烘干过程中'
  '物料内部的温度场与含水率场进行一维径向数值模拟，并据此推算烘干所需的'
  '时间。')

P('第二条　合作方式', indent=False)
P('各方共同参与本软件的开发工作，具体分工如下：')
P('甲方：承担软件总体设计、核心求解算法实现、数值校核与结果验证工作；')
P('乙方：【　　　　　　　　　　　　　　　　　　　　】；')
P('丙方：【　　　　　　　　　　　　　　　　　　　　】。')
P('各方投入的开发工作均为无偿投入，不涉及资金往来。')

P('第三条　开发完成情况', indent=False)
P('本软件于 2026 年 9 月 15 日完成开发。截至该日，软件已经具备完整的'
  '功能，并通过了全部自检项目与结果校核。')
P('本软件尚未以任何形式对外发表。')

P('第四条　著作权归属', indent=False)
P('本软件的著作权由各方共同享有。各方一致同意，在申请软件著作权登记时，'
  '由甲方作为第一著作权人办理登记手续，各方均有权在登记证书中署名。')
P('任何一方不得在未经其他各方书面同意的情况下，单独将本软件的著作权'
  '转让给第三方，或对第三方授予独占性使用许可。')

P('第五条　使用权与收益分配', indent=False)
P('各方均有权在自己承担的教学、科研与工程项目中免费使用本软件。')
P('本软件对外许可使用所产生的收益，由各方按照各自贡献协商分配，'
  '具体比例由各方另行书面约定。')
P('任何一方利用本软件申请科研项目、发表论文、申报奖励的，'
  '应当在相应材料中如实体现其他各方的贡献。')

P('第六条　保密义务', indent=False)
P('本软件的源程序、算法实现细节与技术文档属于各方共同的技术资料。'
  '未经其他各方同意，任何一方不得向第三方披露、提供或允许第三方使用。')

P('第七条　争议解决', indent=False)
P('因本协议引起的争议，各方应先行友好协商解决；协商不成的，'
  '可向甲方所在地人民法院提起诉讼。')

P('第八条　协议生效', indent=False)
P('本协议自各方签字之日起生效。本协议一式【　】份，各方各持一份，'
  '具有同等法律效力。本协议未尽事宜，由各方另行协商并签订补充协议。')

P('')
P('')
P('甲方（签字）：　　　　　　　　　　', indent=False)
P('身份证号：　　　　　　　　　　　　', indent=False)
P('日期：　　年　　月　　日', indent=False)
P('')
P('乙方（签字）：　　　　　　　　　　', indent=False)
P('身份证号：　　　　　　　　　　　　', indent=False)
P('日期：　　年　　月　　日', indent=False)
P('')
P('丙方（签字）：　　　　　　　　　　', indent=False)
P('身份证号：　　　　　　　　　　　　', indent=False)
P('日期：　　年　　月　　日', indent=False)

out = OUTDIR / f'{SOFT_SHORT}_合作开发协议.docx'
doc.save(out)
print('已生成', out)
