# -*- coding: utf-8 -*-
"""截图驱动：把界面摆到指定场景后停住，由外部截窗口。

    python _shot_driver.py <scene> [module]

scene: params | live | result | check
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, r'H:\2026数学建模')
from cpp.python.gui import App                      # noqa: E402
from cpp.python import convert                       # noqa: E402

RUNS = Path(r'H:\2026数学建模\cpp\runs')

scene = sys.argv[1] if len(sys.argv) > 1 else 'params'
module = sys.argv[2] if len(sys.argv) > 2 else 'preheat'

app = App()
app.geometry('1380x880+40+30')
app.module.set(module)
app.load_defaults()


def _load(m):
    p = RUNS / m / 'out.npz'
    if not p.exists():
        return None
    d = np.load(p, allow_pickle=True)
    return {k: d[k] for k in d.files}


if scene == 'params':
    pass                       # 停在参数页

elif scene == 'live':
    app.after(300, app.do_run)  # 真跑一次全周期，看实时曲线

elif scene in ('result', 'check'):
    d = _load(module)
    rd = RUNS / module
    if d is not None:
        s = convert._read_summary(rd)
        app.result.show(module, d, field='C')
        app.check.show(module, rd, s)
        app.log(f'[{module}] 已载入 runs/{module}/out.npz 的结果')
    app.select_tab(scene)

app.mainloop()
