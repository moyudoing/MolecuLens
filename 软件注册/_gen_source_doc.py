# -*- coding: utf-8 -*-
"""生成软著「源程序」文档：每页 50 个渲染行，页眉带软件名称与版本号。

收录顺序按**阅读依赖**排（入口 → 领域层 → 引擎层 → 应用层 → 着色器），
不是文件名字典序 —— 这样前若干页正好是核心数据结构与几何生成代码。
"""
import pathlib

from docx import Document
from docx.enum.text import WD_BREAK, WD_LINE_SPACING
from docx.oxml.ns import qn
from docx.shared import Emu, Pt

ROOT = pathlib.Path(r'F:\MolecuLens')
OUTDIR = ROOT / '软件注册'

SOFT_FULL = 'MolecuLens 分子结构与轨迹显示引擎'
SOFT_SHORT = 'MolecuLens'
VERSION = 'V1.0'
HEADER = f'{SOFT_FULL} {VERSION}'

LINES_PER_PAGE = 50
FRONT_PAGES = 30
BACK_PAGES = 30

# 收录顺序：入口 → 领域层 → 引擎层 → 应用层 → 着色器
ORDER = [
    'CMakeLists.txt', 'vcpkg.json',

    # 领域层：元素表 / 原子 / 场景 / 结构识别码 / 选择语言 / 键搜索
    'src/entity/element/element.h', 'src/entity/element/element.cpp',
    'src/entity/atom/atom.h', 'src/entity/molecule/molecule.h',
    'src/entity/scene/scene.h', 'src/entity/scene/scene.cpp',
    'src/entity/identity/identity.h', 'src/entity/identity/identity.cpp',
    'src/entity/selection/selection.h', 'src/entity/search/bond_search.h',
    'src/entity/search/bond_search.cpp',
    'src/entity/structure/structure_kind.h', 'src/entity/structure/structure_kind.cpp',
    'src/entity/trajectory/trajectory_reader.h',

    # IO：三种格式的解析器
    'src/io/registry/ifile_reader.h', 'src/io/registry/parser_registry.h',
    'src/io/registry/parser_registry.cpp',
    'src/io/pdb/pdb_parser.cpp', 'src/io/gro/gro_parser.cpp',
    'src/io/sdf/sdf_parser.cpp',

    # 引擎层：表示与样式
    'src/engine/rep/representation.h', 'src/engine/style/base_style.h',
    'src/engine/style/style_registry.h', 'src/engine/style/style_registry.cpp',
    'src/engine/rep/representer.h', 'src/engine/rep/representer.cpp',
    'src/engine/style/wireframe/wireframe.cpp',
    'src/engine/style/stick/stick.cpp',
    'src/engine/style/ballstick/ballstick.cpp',
    'src/engine/style/spacefill/spacefill.cpp',
    'src/engine/style/licorice/licorice.cpp',
    'src/engine/style/cartoon/cartoon.cpp',
    'src/engine/style/newcartoon/newcartoon.cpp',
    'src/engine/style/ribbon/ribbon.cpp',
    'src/engine/style/tube/tube.cpp',
    'src/engine/style/surface/surface.cpp',
    'src/engine/style/quicksurf/quicksurf.h',
    'src/engine/style/quicksurf/quicksurf.cpp',
    'src/engine/style/quicksurf/quicksurf_avx2.cpp',

    # 引擎层：命令流 / 相机 / 拾取
    'src/engine/cmd/cmd.h', 'src/engine/cmd/command_list.cpp',
    'src/engine/camera/camera.h',
    'src/engine/camera/camera.cpp', 'src/engine/picking/picking.cpp',
    'src/engine/render/render_parameter.h',

    # 引擎层：Vulkan 后端
    'src/engine/vulkan/device/device.h',
    'src/engine/vulkan/memory/buffer.cpp',
    'src/engine/vulkan/desc/descriptors.cpp',
    'src/engine/vulkan/pipeline/pipeline.cpp',
    'src/engine/vulkan/renderpass/render_pass.cpp',
    'src/engine/vulkan/offscreen/offscreen_renderer.cpp',
    'src/engine/vulkan/realtime/vulkan_renderer.cpp',

    # 引擎层：渲染器与导出
    'src/engine/render/renderer.cpp',
    'src/engine/raytrace/ray_tracer.h', 'src/engine/raytrace/ray_tracer.cpp',
    'src/engine/export/png/png_encoder.cpp',
    'src/engine/scheduler/frame_scheduler.cpp',

    # 应用层：组合根与控制器
    'src/app/main.cpp', 'src/app/shell/application.h',
    'src/app/shell/application.cpp',
    'src/app/events/event.h', 'src/app/events/event_bus.cpp',
    'src/app/data/data_manager.cpp', 'src/app/data/log_buffer.h',
    'src/app/controller/io/io_controller.cpp',
    'src/app/controller/scene/scene_controller.cpp',
    'src/app/controller/rep/representation_controller.h',
    'src/app/controller/rep/representation_controller.cpp',
    'src/app/controller/render/render_controller.cpp',
    'src/app/controller/camera/camera_controller.cpp',
    'src/app/controller/timeline/timeline_controller.cpp',

    # 应用层：界面
    'src/app/view/control/view_control.h',
    'src/app/view/control/view_control.cpp',
    'src/app/view/container/main_view.cpp',
    'src/app/view/viewport/mol_scene.cpp',
    'src/app/view/theme/ui_theme.cpp',
    'src/app/view/timeline/time_axis.cpp',
    'src/app/view/tools/mol_list/mol_list_tool.cpp',
    'src/app/view/tools/styles/styles_tool.cpp',
    'src/app/view/tools/material/material_tool.cpp',
    'src/app/view/tools/export/export_img_tool.cpp',
    'src/app/view/tools/theme/theme_tool.cpp',

    # 命令行
    'src/cli/main.cpp', 'src/cli/cmd/info/info_cmd.cpp',

    # 着色器
    'shaders/sphere.vert', 'shaders/sphere.frag',
    'shaders/cylinder.vert', 'shaders/cylinder.frag',
    'shaders/mesh.vert', 'shaders/mesh.frag',
    'shaders/line.vert', 'shaders/line.frag',
    'shaders/point.vert', 'shaders/point.frag',
    'shaders/global_ubo.glsl',

    # 构建与工具脚本
    'cmake/check_tree.py', 'cmake/spv_to_header.py',
]


def collect():
    out = []
    for rel in ORDER:
        p = ROOT / rel
        if not p.exists():
            print(f'  ! 缺文件 {rel}')
            continue
        out.append(f'// ======== 文件：{rel} ========')
        for ln in p.read_text(encoding='utf-8', errors='ignore').splitlines():
            out.append(ln.rstrip())
        out.append('')
    return out


def style_run(run):
    run.font.name = 'Consolas'
    run.font.size = Pt(9)
    # 中文字形走宋体（Consolas 无中日韩字形，不设的话 Word 回退效果很差）
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
# 正文区宽 = A4 宽 595.28pt − 左右各 1.25in(90pt) = 415.28pt。
# 9pt Consolas 每半角字符约 4.95pt，中文按 2 个半角计；取 80 留余量。
CAP_HALFWIDTH = 80


def disp_width(s):
    return sum(1 if ord(ch) < 128 else 2 for ch in s)


def pack(lines, capacity=CAP_HALFWIDTH):
    """按**渲染行数**分页：每页恰好 LINES_PER_PAGE 个渲染行。

    不能按"每页塞 50 个源文件行"分 —— 长中文注释会折行，
    实际渲染行数超过 50，总页数会比预期多出一截。
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
    sec.page_width, sec.page_height = Emu(7560310), Emu(10692130)
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
    print(f'  前 {len(front)} 页：{nfront} 个源文件行')
    if back:
        print(f'  后 {len(back)} 页：末 {nback} 个源文件行（自第 {total - nback + 1} 行起）')
    else:
        print('  源程序不足 60 页，已全部收录。')


if __name__ == '__main__':
    main()
