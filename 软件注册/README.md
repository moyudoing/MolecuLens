# 软件著作权申请材料

**软件全称**：MolecuLens 分子结构与轨迹显示引擎
**简称**：MolecuLens　　**版本号**：V1.0
**开发完成日期**：2026-09-15　　**发表状态**：未发表

材料依据 `F:\MolecuLens\src` 下的实际代码与实机运行结果编制。
手册中的界面文字、控件名称、参数取值范围与默认值均逐条对照源码，
插图取自程序实际运行（非示意图）。

```
软件注册/
├─ MolecuLens_软著申请表.xlsx       申请表（20 个字段）
├─ MolecuLens_用户手册.docx         文档鉴别材料，12 图 14 表
├─ MolecuLens_源程序.docx           源程序鉴别材料  60 页（前 30 + 后 30）
├─ MolecuLens_合作开发协议.docx     合作开发协议（仅在"合作开发"时需要）
├─ demo/                            演示结构（1HHO 血红蛋白、1UBQ 泛素）
├─ figs/                            插图（实机截图 + 两张结构示意图）
└─ _*.py / _ui.ps1 / _capture.ps1   生成脚本，改内容后重跑即可
```

## 提交前需要补填的内容

| 位置 | 待填项 |
|---|---|
| `MolecuLens_软著申请表.xlsx` | 开发的硬件环境（现填 16GB / 1TB，按实际改） |
| `MolecuLens_用户手册.docx` 封面 | 著作权人名称 |
| `MolecuLens_合作开发协议.docx` | 各方姓名与身份证号、协议份数、签字日期 |

## 关于开发方式：独立还是合作

申请表当前填的是**独立开发**。若本软件确为一人独立完成，则
`MolecuLens_合作开发协议.docx` **不需要提交**，可以删掉。

若实际是多人合作完成，需要改两处后再提交：

1. `_gen_form.py` 顶部把 `'独立开发'` 改成 `'合作开发'`，重跑生成申请表；
2. `_gen_agreement.py` 顶部的 `PARTIES` 里填真实姓名与身份证号，重跑生成协议。

## 关于源程序量

申请表填写的源程序量为 **27327 行**，即 `src/`、`shaders/`、`plugins/`、
`tools/`、`cmake/` 下全部 `.cpp/.h/.cu/.vert/.frag/.glsl/.py` 文件的合计
（不含 `tests/` 下的 1974 行测试代码）。重新统计：

```powershell
python -c "..."   # 见 _linecount.txt，或直接 wc -l
```

中国版权保护中心对源程序的一般要求是**前 30 页 + 后 30 页（共 60 页）**，
当前即按此生成。收录取向按阅读依赖排（入口 → 领域层 → 引擎层 → 应用层 →
着色器），因此前 30 页正好覆盖元素表、场景、解析器与全部表示方式的
几何生成代码，而不是文件名字典序的开头。

## 重新生成

```powershell
$py = "F:\MolecuLens\venv\Scripts\python.exe"
& $py "F:\MolecuLens\软件注册\_gen_form.py"          # 申请表
& $py "F:\MolecuLens\软件注册\_gen_manual.py"        # 用户手册
& $py "F:\MolecuLens\软件注册\_gen_source_doc.py"    # 源程序文档
& $py "F:\MolecuLens\软件注册\_gen_agreement.py"     # 合作协议
& $py "F:\MolecuLens\软件注册\_make_figs.py"         # 面板特写 + 结构示意图
```

## 插图的采集

界面截图由脚本驱动真实程序生成（`_capture.ps1` 调 `_ui.ps1`）：

```powershell
# 前置：应用已启动、窗口 1600x1000、data/ 下有演示结构
& "F:\MolecuLens\软件注册\_capture.ps1"
```

`_ui.ps1` 是一个通用的窗口驱动工具，可单独调用：

```powershell
& _ui.ps1 -Action find                       # 定位窗口与尺寸
& _ui.ps1 -Action resize -W 1600 -H 1000     # 调整窗口大小
& _ui.ps1 -Action click -X 1428 -Y 598       # 点击（窗口相对坐标）
& _ui.ps1 -Action shotclean -Out figs/x.png  # 隐藏输入法条后立即截图
& _ui.ps1 -Action openfile -Text <路径>      # 走"文件→打开"载入结构
```

采集过程中的几个坑（已在脚本里处理，改动时注意别踩回去）：

- **焦点一变就重开下拉框**：`click` 若在已聚焦时仍执行"最小化+还原"，
  会销毁 ImGui 刚打开的弹窗，导致第二次点击落回面板、打到滑杆上。
  脚本已改为"已在前台则直接返回"。
- **输入法状态条**（搜狗 `SoPY_Status`）会盖住标题栏按钮，且随前台
  输入上下文反复复位。必须"隐藏并移出屏幕 → 立即抓屏"一气呵成
  （`-Action shotclean`），中途不能改变前台窗口。
- **截图只能抓屏幕**：Vulkan 窗口走合成器，`PrintWindow` 取不到内容，
  必须 `CopyFromScreen`，因此抓屏前要确保窗口真的在最前。
- **每次采集从新进程开始**：重复运行会累积出多个重叠体系，截图状态不确定。

## 材料里引用的数据出处

| 数据 | 出处 |
|---|---|
| 表示方式种类、半径公式 | `src/engine/rep/representation.h`、`src/engine/style/*/` |
| 界面样式名与配色名 | `src/app/view/tools/styles/styles_tool.cpp` |
| 配色算法（哈希取色相 / BFactor 映射） | `src/engine/rep/representer.cpp` 的 `colorForAtom` |
| 元素表 118 种元素与 CPK 色 | `src/entity/element/element.cpp` |
| 支持的文件格式 | `src/io/registry/parser_registry.cpp` |
| 鼠标操作 | `src/app/view/viewport/mol_scene.cpp` 的 `handleMouse` |
| 导出参数与默认值 | `src/app/view/tools/export/export_img_tool.cpp` |
| 日志环形缓冲（2000 条） | `src/app/data/log_buffer.h` |
| 界面截图 | 实机运行截图（`figs/fig_*.png`） |
| 体系结构图、渲染流程图 | `_make_figs.py` 生成 |

## 演示结构

`figs/` 中的蛋白质插图为 **1HHO**（人血红蛋白，2396 原子，两条肽链 + 血红素），
下载自 RCSB 蛋白质数据库（https://files.rcsb.org/download/1HHO.pdb）；
小分子插图为项目自带的 `data/EPI.pdb`（表儿茶素，73 原子）。
两者都是公开数据，材料中已注明来源。
