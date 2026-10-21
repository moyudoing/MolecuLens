MolecuLens 字体目录
====================

当前字体
--------
`NotoSansSC-Medium-subset.ttf` —— Noto Sans SC（OFL 1.1），已做两处处理：

1. 可变字体实例化到 wght=500（Medium）——ImGui 的 stb_truetype 不会应用变体轴，
   直接用 VF 会退化成默认 wght=100（Thin），界面字太细；
2. 子集化到 8064 个码点（GB2312 全部汉字 + ASCII + 拉丁补充 + 希腊字母 +
   箭头 + 数学算子 + CJK 标点 + 全角符号），2.5 MB（原 VF 17.7 MB）。

许可：SIL Open Font License 1.1，见同目录 `OFL.txt`。子集化的“Modified Version”
主字体名为 `Noto Sans SC Medium`，不含保留字体名 `Source`，符合 OFL 第 3 条。
随应用分发时**必须**同时携带 `OFL.txt` 与版权行（OFL 第 2 条）。

构建与运行
----------
CMake 在构建后把本文件拷贝到可执行目录 `font/NotoSansSC-Medium-subset.ttf`；
运行期候选顺序：项目 `data/font/` → exe 旁 `font/` → 系统字体。
字体缺失时回退系统字体（Windows: 微软雅黑 → 黑体；Linux: Noto CJK；macOS: PingFang SC），
界面仍可用，只是行高与字形略有差异。

复现子集
--------
```powershell
& F:\Python\Python39\python.exe tools\subset_font.py `
    --src "C:/Windows/Fonts/NotoSansSC-VF.ttf" `
    --out data/font/NotoSansSC-Medium-subset.ttf --weight 500
```
脚本说明见 `tools/subset_font.py`：字符集 = `src/app/**/*.cpp|h` 的全部字符
+ GB2312 全字符 + 符号区（拉丁补充 / 希腊 / 箭头 / 数学算子 / CJK 标点 / 全角）。
换字体时改 `--src` 与 `--weight` 即可，无需手工挑字。

历史
----
2026-10-22 之前本目录放的是 `OPPO_B.ttf`——它是汉仪（HANYI KEYIN）构建的 OPPO Sans
v5.00，字体 name 表的许可字段指向 hanyi.com.cn，**不是** SIL OFL；为保证可再分发
与可审计，已换为 Noto Sans SC 子集。旧文件仅留作本地对照（已移出仓库）。
