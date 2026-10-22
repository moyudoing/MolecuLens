# MolecuLens · UI 组件方案（只保留未做项）

> 快照：2026-10-22 02:05（Asia/Shanghai），代码基线 v2.11。
> 范围：设计 token + **尚未实现**的组件与面板改造。P0 五件套（`segmented` / `sliderNum` / `stepper` /
> `badge` / `statChip`）、帮助提示、方屏→满幅视口、检查器加宽可拖动、明暗主题联动背景等已落地，
> 实施进度归档于 [`archive/PLAN-UI组件丰富化-20261022-全量.md`](archive/PLAN-UI组件丰富化-20261022-全量.md)。
> 落点：`src/app/workbench/style/aurora.{h,cpp}`（设计系统）+ `src/app/workbench/modules/modules.cpp`（面板）。

---

## 0. 现状（动手前的基线）

| 类别 | 现有内容 |
|---|---|
| 调色板 | `Palette` 20 个 token（bg/panel/panelAlt/panelSunken/border/borderStrong/text/textMuted/textFaint/accent/accentHi/accentSoft/onAccent/ok/warn/danger/gradTop/gradBottom/dark）+ v2.3 自定义强调色（`setAccent` 推导） |
| 度量 | titleBar 36 / menuBar 36 / fileBar 44 / rail 92（收起 46，按钮 62）/ inspector 420（320–760，可拖）/ quickBar 46 / statusBar 26 / pad 12 |
| 容器 | `cardBegin` / `cardEnd`（成对，**无论返回值都必须 cardEnd**） |
| 控件 | `sectionLabel` / `navButton`（矢量图标）/ `chip` / `toolButton` / `sliderRow` / `toggleRow` / `comboRow` / `inputRow` / `kvRow` / `spacer` / `hline` / `helpTip` / `logoMark` |
| P0 新组件 | `segmented` / `sliderNum` / `stepper` / `badge` / `statChip`（已接入显示面板、材质微调、光照、FOV、播放速度、视口统计） |
| 规模 | `modules.cpp` **2647 行**、`workbench.cpp` 2293 行、`aurora.cpp` 674 行 —— 拆分仍未做 |

布局现状：9 个检查器模块（显示 / 分子 / ESP / 轨道 / 轨迹 / 标注 / 分析 / 日志 / 设置）；左导航 9 项、可收起；
视口满幅铺到检查器左缘（v2.8 去掉"方屏 + 两侧留白"）；浅色主题下背景默认白。

---

## 1. 设计语言（新控件一律遵守）

### 1.1 栅格与节奏
- 基单位 **4 px**；间距只用 4 / 8 / 12 / 16；卡片内边距 12、卡片之间 8、分组之间 16。
- 控件行高 28（与 ImGui FramePadding 对齐），双行列表行高 44。

### 1.2 字号阶梯（复用现有字体，不加新字体文件）
| 用途 | 字号 | 字重 |
| --- | --- | --- |
| 极小标注（单位、序号） | 11 | Regular |
| 正文 / 表单标签 | 12 | Regular |
| 卡片标题 | 13 | Medium |
| 模块标题 | 15 | SemiBold |
| 关键数字（原子数、FPS） | 20 | SemiBold |

### 1.3 形状与交互态
- 圆角：输入框 / 按钮 4，卡片 8，胶囊 999。
- 选中态：`accentSoft` 底 + `accent` 1 px 描边（不用实心，避免抢视线）；主操作才用 `accent` 实心 + `onAccent` 文字。
- hover 底色亮度 +6%，active +12%；disabled 整体 40% 透明但保留 tooltip；focus 用 `borderStrong` 2 px。
- 颜色一律取 `palette()`，**禁止**控件内写死 `IM_COL32`；浅色 / 深色 / 自定义强调色三态都要过一遍。

---

## 2. 未实现的组件（按优先级）

### P0-B（补漏，先做）
| # | 组件 | 签名草案 | 用途 |
|---|---|---|---|
| 1 | `cardGrid` | `bool cardGridBegin(const char* id, int cols); void cardGridEnd();` | 多列卡片容器：检查器 ≥380 px 两列、否则单列；把显示面板纵向长度压下来 |

> P0-A 五件套已完成，不要重写；`cardGrid` 是当时唯一漏掉的 P0。

### P1-A（体验，1–1.5 天）
| # | 组件 | 用途 |
|---|---|---|
| 2 | `accordion` | 带计数与展开箭头的高级分组（"材质微调 3"、"选择 0"）；替代裸 `CollapsingHeader` |
| 3 | `searchBox` | 带清除按钮的搜索框：分子列表、日志过滤、表示列表 |
| 4 | `listRow` | 双行列表项（主标题 + 次行灰字 + 右侧动作），分子实例与表示列表统一（也是 I-24 展开实例的容器） |
| 5 | `emptyState` | 空态占位（矢量图标 + 标题 + 提示 + 主按钮）：空场景、空日志、空轨迹统一 |

### P1-B（专业向，依赖 ESP/表面数据）
| # | 组件 | 用途 |
|---|---|---|
| 6 | `colorRamp` | 色标编辑器：节点增删/拖动 + 渐变预览条 + 反相；ESP 面板必需 |
| 7 | `elementLegend` | 元素图例：色点 + 元素符号 + 原子数，可点击高亮 |
| 8 | `qualityMeter` | 质量条：网格分辨率 / 内存占用 / 三角形预算，带阈值配色 |
| 9 | `kvGrid` | 两列键值网格（替代逐行 `kvRow`，宽屏不浪费） |

### P2（可选，逐项独立提交）
| # | 组件 | 用途 |
|---|---|---|
| 10 | `materialSwatch` | 材质球预览（程序化圆球 + 漫反射/高光/不透明度），点开材质菜单 |
| 11 | `repChipRow` | 表示快选行：类型图标 + 可见性眼睛 + 删除，一眼看全所有表示 |
| 12 | `selectionChip` | 选择表达式胶囊（点击展开语法提示与最近使用） |
| 13 | `miniMap` | 视口缩略图 + 视锥框，点击定位相机 |
| 14 | `timeAxisV2` 剩余部分 | 时间轴两行布局、In/Out、卸载已完成（v2.11）；缺**关键帧标记 / 可拖动标记** |
| 15 | `toast` | 统一通知（成功/警告/错误，3 s 淡出），替代零散报错文本 |

### 其他已识别但未做的小项
1. 分区折叠状态持久化（记住上次展开的「表面 / 材质微调」等）。
2. 下拉框整行可点 + 悬停提示（当前只有箭头区域响应）。
3. 快捷键速查浮层（`?` 唤起；现在只有 F1 帮助/关于）。

---

## 3. 面板改造蓝图（配合上面的组件）

### 3.1 显示（Visualize）
```
[ 表示类型 ]  segmented：球棍 | 空间填充 | 线框 | 表面 | 快速表面
[ 一键风格 ]  segmented：VMD 玻璃 | 不透明
──────────────────────────────────
▾ 几何        原子半径 · 键半径 · 不透明度   （sliderNum）
▾ 材质        materialSwatch + 光泽度/金属度/粗糙度
▾ 表面        等值面 · 网格间距 · 整体系 badge
▾ 作用分子     selectionChip + 语法提示（ISSUES I-26 预校验）
```

### 3.2 分子（Scene）
```
[ searchBox ] 过滤体系/结构/分子名
▾ md（1 体系 · 2042 实例 · 6452 原子） [眼睛] [垃圾桶]     ← accordion + listRow
    SOL ×2032
    ETOH ×1
```
聚合已按「体系 → 结构」两级（v2.10），未做的是搜索、展开实例（I-24）与缓存（I-23）。

### 3.3 ESP / 轨道
`colorRamp`（可编辑 + 反相）· `elementLegend` · `qualityMeter`；轨道面板未来与 ROADMAP §2.2 的 MO 视图共用色标组件。

### 3.4 视口
- 左下 `miniMap`，右下 `statChip` 组（FPS · 三角形 · 内存）。
- 顶部悬浮条维持 `toolButton`；背景开关改用 `segmented`（深色 / 白色 / 透明）。

---

## 4. 验收与约束

**验收**：每个组件单独提交，附浅色/深色/自定义强调色三态截图；面板改动后 `moleculens_app` 构建 exit 0、
`moleculens_tests` 全过（最近记录 184 例 / 193,765 断言），面板滚动高度或操作步数有量化下降。

**约束**
1. 全部控件走 aurora；面板模块里不得出现写死坐标/颜色/间距。
2. 不引入第三方 UI 库（ImGui 单栈），新控件用 `ImDrawList` 现画。
3. 中英双语：所有文案经 `tr()`，控件不接受裸中文。
4. 高 DPI：只用 ImGui 逻辑单位，禁止像素硬编码。
5. 阵型/容器成对 API（`cardBegin` 这类）的 `End` 必须在 `if` 外无条件调用（先例见 `aurora.h` 注释）。
6. 面板性能：组件内避免堆分配（栈缓冲 + `PushID`），面板每帧重绘。
7. 拆分：`modules.cpp` 已 2647 行，后续新增面板优先按模块拆成 `modules_scene.cpp` / `modules_visualize.cpp`，单文件 < 800 行。
