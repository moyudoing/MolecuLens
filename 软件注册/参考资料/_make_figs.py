# -*- coding: utf-8 -*-
"""生成软著材料里的非界面插图：架构图、流程图、结果图。"""
import sys
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

OUT = Path(r'H:\2026数学建模\cpp\软著注册\figs')
OUT.mkdir(parents=True, exist_ok=True)
RUNS = Path(r'H:\2026数学建模\cpp\runs')

plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False

C_UI = '#cfe3f7'
C_PY = '#d8ecd6'
C_CPP = '#fbe0c8'
C_IO = '#eeeeee'
C_EDGE = '#4a6b8a'


def box(ax, x, y, w, h, text, fc, fs=10, weight='normal'):
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                boxstyle='round,pad=0.012,rounding_size=0.02',
                                fc=fc, ec=C_EDGE, lw=1.1))
    ax.text(x + w / 2, y + h / 2, text, ha='center', va='center',
            fontsize=fs, fontweight=weight, linespacing=1.5)


def arrow(ax, p, q, style='-|>', color=C_EDGE, ls='-', lw=1.3):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle=style, color=color,
                                 lw=lw, linestyle=ls,
                                 mutation_scale=13,
                                 shrinkA=1, shrinkB=1))


# ==========================================================================
# 图 1  软件架构
# ==========================================================================
def fig_arch():
    fig, ax = plt.subplots(figsize=(9.2, 6.4))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')

    ax.text(0.5, 0.965, '软件体系结构', ha='center', fontsize=15,
            fontweight='bold')

    # 表现层
    ax.text(0.035, 0.885, '表现层', fontsize=10.5, color='#33506b',
            fontweight='bold')
    box(ax, 0.16, 0.828, 0.34, 0.098, '图形界面\n（tkinter + matplotlib）', C_UI)
    box(ax, 0.53, 0.828, 0.33, 0.098, '命令行入口\ncli.py', C_UI)

    # 编排层
    ax.text(0.035, 0.745, '编排层', fontsize=10.5, color='#33506b',
            fontweight='bold')
    for i, t in enumerate(['参数装配\nspecs', '输入表生成\nprep_tables',
                           '结果组装\nconvert', '对比验证\nverify']):
        box(ax, 0.16 + i * 0.2, 0.675, 0.175, 0.098, t, C_PY, fs=9)

    # 交换层
    box(ax, 0.16, 0.505, 0.70, 0.088,
        '数据交换：params.txt（参数）→  求解  →  CSV（场/序列/守恒）',
        C_IO, fs=9.5)
    arrow(ax, (0.51, 0.673), (0.51, 0.596), ls='--')
    arrow(ax, (0.51, 0.503), (0.51, 0.455))

    # 求解层
    ax.text(0.035, 0.395, '求解层', fontsize=10.5, color='#33506b',
            fontweight='bold')
    ax.add_patch(FancyBboxPatch((0.16, 0.115), 0.70, 0.335,
                                boxstyle='round,pad=0.012,rounding_size=0.02',
                                fc=C_CPP, ec=C_EDGE, lw=1.3))
    ax.text(0.51, 0.408, 'cpp_sim.exe    C++17 核心求解器', ha='center',
            fontsize=10.5, fontweight='bold')
    for i, t in enumerate(['网格与物性\n三套物性方案', 'Crank–Nicolson\n+ Picard 迭代',
                           '两阶段液膜\n表面模型', '移动边界\n（ξ 坐标）']):
        box(ax, 0.185 + i * 0.168, 0.15, 0.15, 0.20, t, 'white', fs=8.8)

    # 底：外部输入
    box(ax, 0.16, 0.012, 0.335, 0.072, '工况曲线：烘房温湿度', C_IO, fs=9)
    box(ax, 0.525, 0.012, 0.335, 0.072, '收缩曲线：药材半径', C_IO, fs=9)

    fig.tight_layout()
    fig.savefig(OUT / 'fig_arch.png', dpi=170)
    plt.close(fig)
    print('fig_arch.png')


# ==========================================================================
# 图 2  计算流程
# ==========================================================================
def fig_flow():
    fig, ax = plt.subplots(figsize=(9.4, 7.4))
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    ax.text(0.5, 0.962, '求解流程', ha='center', fontsize=15, fontweight='bold')

    BLUE, AMBER, GREY = '#e8f0fb', '#fde9c8', '#efefef'

    # ---- 循环之前 ----
    box(ax, 0.22, 0.888, 0.56, 0.054, '读入工况曲线，建立边界条件', GREY, fs=9.6)
    box(ax, 0.22, 0.812, 0.56, 0.054, '构造径向网格与物性参数', GREY, fs=9.6)
    arrow(ax, (0.50, 0.888), (0.50, 0.866))
    arrow(ax, (0.50, 0.812), (0.50, 0.786))

    # ---- 时间步循环（虚线框）----
    ax.add_patch(FancyBboxPatch((0.075, 0.198), 0.85, 0.578,
                                boxstyle='round,pad=0.012,rounding_size=0.02',
                                fc='#fafafa', ec='#9bb0c4', lw=1.2,
                                linestyle='--'))
    ax.text(0.100, 0.748, '每个时间步', fontsize=10, color='#33506b',
            fontweight='bold')

    inner = ['组装三对角系数（隐式 / 显式半边分开）',
             '解质量方程，更新含水率场',
             '按新含水率更新容量与导热系数',
             '解能量方程，更新温度场']
    ys = [0.676, 0.604, 0.532, 0.460]          # 顶边；框高 0.050
    for t, y in zip(inner, ys):
        box(ax, 0.20, y, 0.62, 0.050, t, BLUE, fs=9.2)
    for y in ys[:-1]:
        arrow(ax, (0.51, y), (0.51, y - 0.022))

    # ---- Picard 判据 ----
    ydec = 0.364
    box(ax, 0.30, ydec, 0.42, 0.050, 'Picard 迭代收敛？', AMBER, fs=9.5)
    arrow(ax, (0.51, ys[-1]), (0.51, ydec + 0.050))

    # 否：回到「组装三对角系数」
    arrow(ax, (0.30, ydec + 0.025), (0.135, ydec + 0.025), ls='--',
          color='#b06a2c')
    arrow(ax, (0.135, ydec + 0.025), (0.135, ys[0] + 0.025), ls='--',
          color='#b06a2c')
    arrow(ax, (0.135, ys[0] + 0.025), (0.20, ys[0] + 0.025), ls='--',
          color='#b06a2c')
    ax.text(0.145, (ydec + ys[0]) / 2, '否', fontsize=9, color='#b06a2c')

    # 是：出循环
    arrow(ax, (0.51, ydec), (0.51, 0.170))
    ax.text(0.527, ydec - 0.030, '是', fontsize=9, color='#b06a2c')

    # ---- 循环之后 ----
    box(ax, 0.20, 0.112, 0.62, 0.054, '记录快照 / 守恒追踪 / 终点判据', BLUE,
        fs=9.4)
    arrow(ax, (0.51, 0.112), (0.51, 0.088))
    box(ax, 0.20, 0.032, 0.62, 0.054, '推进下一时间步，直至结束', BLUE, fs=9.4)

    fig.subplots_adjust(left=0.02, right=0.98, top=0.99, bottom=0.01)
    fig.savefig(OUT / 'fig_flow.png', dpi=170)
    plt.close(fig)
    print('fig_flow.png')


# ==========================================================================
# 图 3  各模块干燥曲线
# ==========================================================================
def fig_drying():
    fig, axes = plt.subplots(1, 2, figsize=(10.2, 4.0))
    ax1, ax2 = axes

    for q, lab, c in (('preheat', '预热阶段', '#1f77b4'),
                      ('fullcycle', '全周期', '#2ca02c'),
                      ('duration', '时长预测', '#d62728'),
                      ('shrinkage', '收缩工况', '#9467bd')):
        p = RUNS / q / 'out.npz'
        if not p.exists():
            continue
        d = np.load(p, allow_pickle=True)
        if q == 'preheat':
            th = d['t'] / 3600.0
            cc = d['C'][:, 0]
            cs = d['C'][:, -1]
        elif 'row_t' in d.files and len(d['row_t']):
            th = d['row_t'] / 3600.0
            cc = d['row_C'][:, 0]
            cs = d['row_C'][:, -1]
        else:
            continue
        ax1.plot(th, cc, lw=1.5, color=c, label=lab)
        ax2.plot(th, cs, lw=1.5, color=c, label=lab)

    for ax, ttl in ((ax1, '中心含水率'), (ax2, '表面含水率')):
        ax.axhline(0.15, color='crimson', ls='--', lw=1,
                   label='终点判据 0.15')
        ax.set_xlabel('时间 / h')
        ax.set_ylabel('含水率 / (kg/kg)')
        ax.set_title(ttl)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    ax1.set_xlim(0, 75)
    ax2.set_xlim(0, 75)
    fig.tight_layout()
    fig.savefig(OUT / 'fig_drying.png', dpi=170)
    plt.close(fig)
    print('fig_drying.png')


# ==========================================================================
# 图 4  场演化云图（全周期模块）
# ==========================================================================
def fig_field():
    p = RUNS / 'fullcycle' / 'out.npz'
    if not p.exists():
        print('缺 fullcycle/out.npz，跳过 fig_field')
        return
    d = np.load(p, allow_pickle=True)
    r = d['r'] * 100.0
    th = d['t'] / 3600.0
    fig, axes = plt.subplots(1, 2, figsize=(10.4, 4.0))
    for ax, Z, lab, cmap in ((axes[0], d['T'] - 273.15, '温度 / ℃', 'inferno'),
                             (axes[1], d['C'], '含水率 / (kg/kg)', 'viridis_r')):
        im = ax.pcolormesh(th, r, Z.T, shading='auto', cmap=cmap)
        fig.colorbar(im, ax=ax, label=lab, pad=0.02)
        ax.axvline(4.0, color='w', ls='--', lw=1)
        ax.text(4.4, 1.85, '恒温段起点 4 h', color='w', fontsize=8)
        ax.set_xlabel('时间 / h')
        ax.set_ylabel('距中心距离 / cm')
    axes[0].set_title('温度场演化')
    axes[1].set_title('水分场演化')
    fig.tight_layout()
    fig.savefig(OUT / 'fig_field.png', dpi=170)
    plt.close(fig)
    print('fig_field.png')


if __name__ == '__main__':
    fig_arch()
    fig_flow()
    fig_drying()
    fig_field()
    print('完成 ->', OUT)
