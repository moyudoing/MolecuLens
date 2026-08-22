MolecuLens 字体目录
====================

主方案 §7.6：字体使用 OPPO B.ttf（exe 同级 font/，CMake 后构建步骤拷贝），
中文 Glyph Ranges；三档字号 16px / 20px / 13px。

OPPO B.ttf 为 OPPO 官方字体（SIL Open Font License 1.1 的子集授权，可随应用分发）。
若未随仓库提供，构建时会回退到系统字体：
  - Windows: msyh.ttc（微软雅黑）
  - Linux:   Noto Sans CJK
  - macOS:   PingFang SC

将 OPPO_B.ttf 放入本目录后重新构建即可生效。
