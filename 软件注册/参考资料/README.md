# 软件著作权申请材料

**软件全称**：药材烘干过程数值模拟软件
**简称**：烘干模拟　　**版本号**：V1.0
**开发完成日期**：2026-09-15　　**发表状态**：未发表　　**开发方式**：合作开发

材料依据 `cpp/` 下的实际代码与运行结果编制，数据均为真实计算结果。

```
软著注册/
├─ 烘干模拟_软著申请表.xlsx        申请表（20 个字段）
├─ 烘干模拟_源程序.docx            源程序鉴别材料  50 页（前 25 + 后 25）
├─ 烘干模拟_用户手册.docx          文档鉴别材料    19 页，10 图 9 表
├─ 烘干模拟_合作开发协议.docx      合作开发协议（待填空）
├─ figs/                           插图源文件（6 张界面截图 + 4 张生成图）
├─ 预览_用户手册.pdf / 预览_源程序.pdf   排版预览（不用于提交）
└─ _gen_*.py / _capture.ps1        生成脚本，改内容后重跑即可
```

## 需要你补填的内容

以下属于个人信息或需要你确认的事实，脚本里一律留成 `【　】`：

| 位置 | 待填项 |
|---|---|
| `烘干模拟_软著申请表.xlsx` | 开发的硬件环境（现填 16GB / 1TB，按实际改） |
| `烘干模拟_合作开发协议.docx` | 各方的**姓名**与**身份证号**、签字日期、协议份数 |
| `烘干模拟_用户手册.docx` 封面 | 著作权人名称 |

参与人数与模板不是三人时，在 `_gen_agreement.py` 里增删对应段落重跑即可。

## 功能模块与命名

材料与源码统一按功能命名，不使用任何题目式编号：

| 模块标识 | 名称 | 用途 |
|---|---|---|
| `preheat` | 预热阶段模拟 | 升温与表面失水的短时段 |
| `fullcycle` | 全周期干燥模拟 | 完整烘干过程的场演化 |
| `duration` | 干燥时长预测 | 推送到全域达标，给出所需时间 |
| `shrinkage` | 收缩工况模拟 | 半径随失水收缩的情形 |

物性方案：`props_a`（常数物性）、`props_b`（质量分数混合律）、
`props_c`（低导热变物性）。

## 关于页数

源程序文档按**每页 50 个渲染行**分页，共 50 页（前 25 + 后 25），
收录 7687 行中的前 1213 行与末 1186 行。

⚠ 中国版权保护中心对源程序的一般要求是**前 30 页 + 后 30 页（共 60 页）**。
当前按要求做的是 25 + 25 = 50 页。若受理窗口按 60 页把关，
把 `_gen_source_doc.py` 顶部的 `FRONT_PAGES` / `BACK_PAGES` 改成 30 重跑即可。

源程序量（申请表填写用）= **7687 行**，其中 C++ 与 Python 合计约 7500 行，
构建脚本 150 余行。

## 重新生成

```powershell
cd H:\2026数学建模
python cpp\软著注册\_gen_form.py          # 申请表
python cpp\软著注册\_gen_source_doc.py    # 源程序文档
python cpp\软著注册\_gen_manual.py        # 用户手册
python cpp\软著注册\_gen_agreement.py     # 合作协议
python cpp\软著注册\_make_figs.py         # 架构图 / 流程图 / 结果图
```

界面截图由 `_capture.ps1` 驱动生成（会真的启动界面并截取窗口）：

```powershell
cd H:\2026数学建模\cpp\软著注册
.\_capture.ps1 -Scene params -Qid preheat   -Out figs\gui_params_preheat.png   -WaitSec 9
.\_capture.ps1 -Scene live   -Qid fullcycle -Out figs\gui_live_fullcycle.png   -WaitSec 28 -Watch
```

`-Scene` 可选 `params` / `result` / `check` / `live`；
`-Watch` 表示该场景要真跑一次求解（`live` 用），等待时间要够长。

## 材料里引用的数据出处

| 数据 | 出处 |
|---|---|
| 预热阶段 表面 36.7731 ℃、含水率 1.5103 | `cpp/runs/preheat/out.npz` |
| 全周期 72 h 场演化 | `cpp/runs/fullcycle/out.npz` |
| 时长预测 59.2771 h | `cpp/runs/duration/out.npz` |
| 收缩工况 53.6104 h、分解 132.87 / 26.56 | `cpp/runs/shrinkage/out.npz` |
| 守恒残差、校核偏差 | `cpp/runs/*/summary.txt`、`budget.csv` |
| 界面截图 | 实机运行截图（`figs/gui_*.png`） |
| 架构图、流程图、结果图 | `_make_figs.py` 生成 |
