# -*- coding: utf-8 -*-
"""生成软著材料里除整窗截图之外的插图。

分两类：
  1) 面板特写 —— 从整窗截图里裁出单个面板（渲染设置 / 分子列表 / 日志 …）；
  2) 结构图   —— 用 matplotlib 画的软件体系结构与渲染流程示意图。
"""
import pathlib

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
from PIL import Image

BASE = pathlib.Path(r'F:\MolecuLens\软件注册')
FIGS = BASE / 'figs'

plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False

C_UI = '#cfe3f7'
C_ENG = '#d8ecd6'
C_GPU = '#fbe0c8'
C_IO = '#eeeeee'
C_EDGE = '#4a6b8a'


def crop(name, src, box, out):
    """从整窗截图裁出面板特写。box = (left, top, right, bottom)"""
    im = Image.open(FIGS / src).convert('RGB').crop(box)
    im.save(FIGS / out)
    print('  %-28s <- %s %s' % (out, src, box))


def box(ax, x, y, w, h, text, fc, fs=10, weight='normal'):
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                boxstyle='round,pad=0.012,rounding_size=0.02',
                                fc=fc, ec=C_EDGE, lw=1.1))
    ax.text(x + w / 2, y + h / 2, text, ha='center', va='center',
            fontsize=fs, fontweight=weight, linespacing=1.5)


def arrow(ax, p, q, color=C_EDGE, ls='-', lw=1.3):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle='-|>', color=color,
                                 lw=lw, linestyle=ls, mutation_scale=13,
                                 shrinkA=1, shrinkB=1))


def fig_layers():
    """软件体系结构：四层，依赖单向。"""
    fig, ax = plt.subplots(figsize=(9.2, 6.2))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    ax.text(0.5, 0.965, '软件体系结构', ha='center', fontsize=15, fontweight='bold')

    ax.text(0.03, 0.882, '表现层', fontsize=10.5, color='#33506b', fontweight='bold')
    box(ax, 0.15, 0.822, 0.36, 0.098, '主视图与面板\n（自绘标题栏 / Dock / 视口）', C_UI, fs=9)
    box(ax, 0.55, 0.822, 0.32, 0.098, '视图控制器\n（面板路由 / 布局 / 日志读取）', C_UI, fs=9)

    ax.text(0.03, 0.742, '应用层', fontsize=10.5, color='#33506b', fontweight='bold')
    for i, t in enumerate(['场景控制器', '表示控制器', '渲染控制器', '相机控制器',
                           'IO 控制器', '时间轴控制器']):
        box(ax, 0.15 + i * 0.12, 0.672, 0.108, 0.098, t, C_ENG, fs=8.2)

    ax.text(0.03, 0.592, '引擎层', fontsize=10.5, color='#33506b', fontweight='bold')
    box(ax, 0.15, 0.498, 0.72, 0.088,
        '表示与样式（十三种表示）· 命令流 · 相机与拾取 · 光线追踪', C_ENG, fs=9.2)
    box(ax, 0.15, 0.398, 0.72, 0.088,
        'Vulkan 渲染后端（离屏视口 / 主交换链 / 管线与描述符）', C_GPU, fs=9.2)

    ax.text(0.03, 0.318, '领域层', fontsize=10.5, color='#33506b', fontweight='bold')
    box(ax, 0.15, 0.222, 0.72, 0.088,
        '场景与体系 · 原子与元素表 · 结构识别码 · 选择语言 · 轨迹', C_IO, fs=9.2)
    box(ax, 0.15, 0.118, 0.72, 0.088,
        '文件解析（PDB / GRO / SDF）· 持久化与设置', C_IO, fs=9.2)

    for y in (0.822, 0.672, 0.498, 0.398, 0.222):
        arrow(ax, (0.51, y - 0.004), (0.51, y - 0.022))
    fig.tight_layout()
    fig.savefig(FIGS / 'fig_layers.png', dpi=170)
    plt.close(fig)
    print('  fig_layers.png')


def fig_pipeline():
    """从打开文件到画面上屏的处理流程。"""
    fig, ax = plt.subplots(figsize=(9.4, 7.0))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    ax.text(0.5, 0.965, '渲染流程', ha='center', fontsize=15, fontweight='bold')

    GREY, BLUE, AMBER = '#efefef', '#e8f0fb', '#fde9c8'

    box(ax, 0.20, 0.876, 0.60, 0.056, '选择结构文件（PDB / GRO / SDF）', GREY, fs=9.6)
    arrow(ax, (0.50, 0.876), (0.50, 0.852))
    box(ax, 0.20, 0.796, 0.60, 0.056, '后台线程解析：原子 / 键 / 残基 / 链 / 帧', GREY, fs=9.6)
    arrow(ax, (0.50, 0.796), (0.50, 0.772))
    box(ax, 0.20, 0.716, 0.60, 0.056, '合并进场景，建立一个体系并居中', GREY, fs=9.6)
    arrow(ax, (0.50, 0.716), (0.50, 0.692))
    box(ax, 0.20, 0.636, 0.60, 0.056, '相机取景（按包围球设距离与近远面）', GREY, fs=9.6)
    arrow(ax, (0.50, 0.636), (0.50, 0.608))

    ax.add_patch(FancyBboxPatch((0.07, 0.198), 0.86, 0.396,
                                boxstyle='round,pad=0.012,rounding_size=0.02',
                                fc='#fafafa', ec='#9bb0c4', lw=1.2, linestyle='--'))
    ax.text(0.095, 0.548, '场景变更时重建命令流', fontsize=10,
            color='#33506b', fontweight='bold')
    rows = ['按表示类型生成几何：球 / 圆柱 / 线段 / 三角网格',
            '按配色方案决定每个原子的颜色',
            '生成命令缓冲（材质、渲染模式、几何批次）']
    ys = [0.478, 0.398, 0.318]
    for t, y in zip(rows, ys):
        box(ax, 0.19, y, 0.62, 0.056, t, BLUE, fs=9.2)
    for y in ys[:-1]:
        arrow(ax, (0.50, y), (0.50, y - 0.024))

    arrow(ax, (0.50, 0.318), (0.50, 0.268))
    box(ax, 0.19, 0.212, 0.62, 0.056, '提交 GPU 绘制到离屏画布', AMBER, fs=9.4)

    arrow(ax, (0.50, 0.212), (0.50, 0.166))
    box(ax, 0.20, 0.110, 0.60, 0.056, '画布作为纹理贴回视口，与面板一同呈现', BLUE, fs=9.6)
    arrow(ax, (0.50, 0.110), (0.50, 0.064))
    box(ax, 0.20, 0.008, 0.60, 0.056, '也可离屏导出为 PNG（光栅化 / 光线追踪）', GREY, fs=9.6)

    fig.subplots_adjust(left=0.02, right=0.98, top=0.99, bottom=0.01)
    fig.savefig(FIGS / 'fig_pipeline.png', dpi=170)
    plt.close(fig)
    print('  fig_pipeline.png')


def main():
    print('面板特写：')
    # 渲染设置面板（含样式 / 着色 / 缩放 / 背景 / 雾效）
    crop('render', 'fig_color_chain.png', (1285, 490, 1600, 1000), 'fig_panel_render.png')
    # 分子列表（体系树）
    crop('molist', 'fig_main.png', (1285, 60, 1600, 480), 'fig_panel_molist.png')
    # 日志面板
    crop('log', '_log.png', (0, 726, 960, 800), 'fig_panel_log.png')

    print('结构图：')
    fig_layers()
    fig_pipeline()
    print('完成 ->', FIGS)


if __name__ == '__main__':
    main()
