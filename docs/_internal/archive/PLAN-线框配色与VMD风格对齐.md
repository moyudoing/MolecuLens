# PLAN · 线框配色与 VMD 风格对齐（只读分析，2026-10-21 13:4x）

> 前提：本文基于 **13:00–13:24 桌面会话已落地的改动**（`representation.h` / `representer.cpp` /
> `wireframe.cpp` / `scene_bridge.cpp` / `render_cmd.cpp` / `modules.cpp` / `language.cpp` /
> `selection.cpp`）重新核对，不是 12:40 交接文档的旧快照。本文只写方案，未改任何源码。
> 唯一写入：本文件。

---

## 0. 结论（先看这段）

1. **「一个链只要一个颜色」不是单一缺陷，是三个问题叠加**，13:09 的改动只修掉其中一个：
   | # | 根因 | 位置 | 现状 |
   |---|---|---|---|
   | R1 | 每条键只用 **a 端原子**的颜色（线段只有 1 个 color 字段） | 旧 `wireframe.cpp:48` | **已修**（半键拆分，`wireframe.cpp:44-61`） |
   | R2 | `ColorScheme::Chain` 语义就是「每链一常量色」 | `representer.cpp:126-132`（**原样未动**） | 设计使然，但 md.gro 上必然"整盒一色" |
   | R3 | **GRO 把残基名当链**：md.gro 只有 7 个伪链，SOL 占 6096/6452 = 94.5% 原子 | `gro_parser.cpp:186-196`、`:311-312` | **未修**，这是"整盒同色"的直接原因 |
2. 13:00–13:24 真正交付的是**「VMD Name 观感」这第三种配色**（`ColorScheme::AtomName`）+ 半键拆分，
   属于「线框默认改元素/原子名配色」方案的一半：**有选项、没默认**（默认仍是 `ElementCPK`，
   `representation.h:50`；`setType` 不会随样式切配色，`representation_controller.cpp:87-120`）。
3. 该轮改动**引入了 3 个新问题**（都不在 src 的"主路径"上，但会踩到）：
   P1 `colorSchemeZhImpl` 缺 `AtomName` 分支 → 返回"未知"（`representer.cpp:36-45`）；
   P2 旧「图形表示」面板配色下拉仍是 **5 项**，`colorIdx=5` 时 `colorNames[5]` **越界读**
   （`reps_tool.cpp:375-377`，该面板由 `view_control.cpp:375` 实例化，是活代码）；
   P3 调色板按 sRGB 手写、管线按**线性**解释并硬件编码 → 全部颜色偏淡（见 §1.4 实测）。
4. 推荐：**方案 A（低风险，先做）+ 方案 B（默认值，1 行）**；方案 C（sRGB→linear）单独排一轮，
   因为它会让所有既有截图/阈值失效。

---

## 1. 现状诊断（证据）

### 1.1 Chain 配色到底怎么算
```cpp
// representer.cpp:126-132（13:07 之后依然是这段，未改）
case ColorScheme::Chain: {
    const int32_t cid = st.atoms[localIdx].chainId;
    const float h = float(hash32(uint32_t(cid + 1)) % 3600) / 3600.0f;
    const glm::vec3 rgb = hslToRgb(h, 0.65f, 0.55f);   // 固定 s/l，只变色相
    return glm::vec4(rgb, 1.0f);
}
```
- **每链一个常量色**：色相由 `hash32(chainId+1)` 决定，同一 `chainId` 的所有原子取到同一色。
  这是 VMD `ColorID`/`Chain` 方法的等价设计，**本身不是 bug**。
- 但它是「哈希取色」而非「表驱动」：`% 3600` 意味着**相邻 chainId 的色相不做拉开**，7 个链可能撞到相近色相。
- `ColorScheme::Residue`（`:133-139`）同构，且同样只按 `residueId`（= 残基**名**的池下标，见 §1.3）。

### 1.2 线框到底走哪条绘制路径
```
RepresentationType::Wireframe(0)
  → style_registry.h:34（编译期静态表 kStyleStaticTable）/ styleFor()：style_registry.cpp:72-78
  → base_style.h:27  drawWireframe
  → wireframe.cpp:13  drawWireframe：遍历 st.bonds，每键 append Cmd::LineInst
  → cmd.h:79-84  LineInst{ aPos(vec4), bPos(vec4), color(vec4) } = 48B，**只有一个 color**
  → decode.cpp:123-135 → DrawList::lines/lineMat
  → draw_geometry.cpp:253-262  每段展开为 (aPos,color, bPos,color) 两个顶点（32B/顶点）
  → pipeline.cpp:305  VK_PRIMITIVE_TOPOLOGY_LINE_LIST（line 管线，无光照）
  → line.frag：outColor = mix(vColor.rgb, fogColor(), depthCueFactor(vDist))，**line.frag 不用材质**
```
- 关键：**LineInst 没有双端颜色**，所以"一根键两色"只能靠**拆两段**实现——
  这正是 13:09 的做法（`wireframe.cpp:47-61`，中点 `mid`，两段各用本端原子色）。
- 另一条容易混淆的"线框"：`SystemRenderMode::Wireframe`（`cmd.h:145-149`，renderMode=1）走的是
  `VK_POLYGON_MODE_LINE` 管线（`pipeline.cpp:255`/`:283`，sphere-wire / cylinder-wire），
  那是**多边形线框**，与"线状表示"是两件事。用户说的"线框显示"= 前者（表示=线状）。

### 1.3 md.gro 为什么必然"整盒一色"
```cpp
// gro_parser.cpp:186-196 —— 注释原文：「残基名/链名去重表（按残基名做 chain，与溶剂分组一致）」
auto chainIdFor = [&](const std::string& name) -> int32_t { ... scene->addChain(name) ... };
// gro_parser.cpp:197-205 —— residueIdFor 同样按「残基名」去重
// gro_parser.cpp:311-312
const int32_t rid = residueIdFor(mol.resName);
const int32_t cid = chainIdFor(mol.resName);
```
实测 `data/md.gro`（6452 原子 / 2042 分子）的残基名直方图：
`SOL 6096`、`EPI 140`、`R 88`、`T 34`、`NAR 32`、`Q 32`、`API 30`。
→ **「按链」只有 7 种颜色，其中 SOL 一种颜色覆盖 94.5% 的原子**；
→ 更隐蔽的是「按残基」同样只有 7 种颜色（residueId 也是按名字去重的池下标），
  VMD 的 `Residue` 是**逐 resid**（每个水分子一色），语义上本引擎这里与 VMD 不一致。

**结论定级**：
- 有真实链的 PDB（`pdb_parser.cpp:142` 取 chainID 列）→「按链」= 一链一色 = **VMD 一致的设计**；
- GRO / 溶剂盒子 / 多实例体系 → 退化为"按残基名" = **实现缺陷（R3）**，符合用户"一个链只要一个颜色"的体感。

### 1.4 定量证据（可复现）
对 `_test/vmd/` 现有出图做像素直方图（PowerShell + System.Drawing，非白像素按 8 级量化）：

| 图 | 非白像素 | 量化后颜色数 | 主要颜色 |
|---|---|---|---|
| `our_wireframe.png`（12:35，改动前） | 23,661 | **2** | `rgb(255,64,64)` 93.7%，`rgb(200,200,200)` 6.3% |
| `vmd_ref_surf_conv.png` | 192,847 | 482 | 水壳粉 `rgb(192,160,160)` 系；溶质青 ≈ `rgb(72,96,96)`；溶质 O ≈ `rgb(120,32,32)` |

两条重要推论：
1. 那张自家线框图是 **ElementCPK**，不是「按链」：红=O（`element.cpp:31` `1.00,0.05,0.05`），
   灰=C（`:29` `0.56` 灰），**H 是纯白（`:24` `1,1,1`）→ 白底上完全不可见**。
   而旧实现每键只有 O 的颜色 → 「O–H 键通体红」→ 视觉上就是"一根键/一整片只有一个颜色"。
   **这才是用户那句抱怨的直读证据（R1），R2/R3 是他在 GUI 里切「按链」时看到的第二层。**
2. 颜色空间对不上：颜色附件是 `*_SRGB`（`offscreen_renderer.h:104-107`、`device.h:157`），
   顶点色被当作**线性**值写入 → 硬件做 linear→sRGB 编码。
   sRGB 编码校验：`encode(0.05)=0.2477→63`≈实测 **64**；`encode(0.56)=0.7735→197`≈实测 **200**，完全吻合。
   → 调色板（`element.cpp` 表、`representer.cpp:149-166` 的 AtomName 常量）是**按 sRGB 手写**的显示值，
   未经 sRGB→linear 反变换就送进管线，于是**整体偏淡**：O 显示成粉红 `(255,64,64)` 而非 `(255,13,13)`，
   AtomName 的碳青 `(0.55,0.85,0.88)` 屏显 ≈ `(195,238,241)`（近白），而 VMD 参考图里溶质青是 `(72,96,96)`。
   `cmd.h:61` 注释写「RGBA（sRGB 预计算）」与实现相反 → **文档/实现不一致，建议同轮修注释**。

### 1.5 13:00–13:24 这轮实际改了什么（核对结果）
| 文件 | 改动实质 | 评价 |
|---|---|---|
| `representation.h:35-43` | 新增 `ColorScheme::AtomName`（枚举第 5 位，`Count` 由 5→6） | ✓ 与 VMD Name 对齐的第一步 |
| `representer.cpp:145-168` | `AtomName` 分支：C 青/N 蓝/O 红/H 白/S 黄/P 橙，其余回落 CPK | ✓ 但常量是 sRGB 值（见 §1.4-2） |
| `wireframe.cpp:44-61` | 每键拆两段，各用本端原子色（VMD Lines 做法） | ✓ **直接修掉 R1** |
| `scene_bridge.cpp:76-81` | 持久化键 `{"name"}`；`:166-168`/`:475` 补原子名快照 | ✓ 旧文件无该键 → 回落 element，兼容 |
| `render_cmd.cpp:66-80`、`:194-199` | CLI `--color name` + 帮助 | ✓ |
| `modules.cpp:105-106`、`:185-197` | Workbench 配色下拉 6 项 | ✓ 枚举下标对齐 |
| `language.cpp:217` | i18n「按原子名 (VMD)」 | ✓ |
| `selection.cpp:49` | 选择语言新增 `name` 属性 | ✓ 独立特性 |

**未改**：`representer.cpp` 的 `Chain`/`Residue` 分支、`gro_parser.cpp` 的链映射、`setType` 的配色默认、
`reps_tool.cpp` 的 5 项下拉、`colorSchemeZhImpl`。

---

## 2. 缺口清单（按优先级）

| # | 缺口 | 证据 | 影响 |
|---|---|---|---|
| G1 | `colorSchemeZhImpl` 无 `AtomName` 分支 → "未知" | `representer.cpp:36-45`；显示处 `reps_tool.cpp:317` | UI 显示"未知"，CLI 日志/截图不可读 |
| G2 | 旧面板配色下拉 5 项，越界读 | `reps_tool.cpp:375-377`（`IM_ARRAYSIZE` 未跟随枚举） | 选中 AtomName 后再开该面板 → 读 `colorNames[5]`（未定义行为，可能崩/花屏） |
| G3 | 线框默认配色没跟着变 | `representation.h:50`、`representation_controller.cpp:87-120` | 新开线框仍是 CPK：白底 H 不可见、灰碳+红氧对比弱 |
| G4 | Chain 在 GRO/溶剂盒退化为"残基名色" | `gro_parser.cpp:186-196`、`:311-312` | 用户抱怨的"整盒一色"仍会出现（切到「按链」即可复现） |
| G5 | 调色板缺 sRGB→linear | §1.4-2 实测 | 所有颜色偏淡，与 VMD 参考图色差大 |
| G6 | 白底 + H=纯白 | `element.cpp:24`、`representer.cpp:158-160`（0.95 近白） | 水盒线框丢掉一半线段（H），构图"稀" |

> 注意：G1/G2 是**回归风险**（这轮改动带出来的），建议在下一轮开工时先补，代价 ≤ 10 行。

---

## 3. 改造方案

### 方案 A（推荐先做）：Chain 语义补强 ——「表驱动色板 + 链内亮度扰动」
不改枚举、不改持久化键，只改 `representer.cpp` 两个分支。

```cpp
// representer.cpp 匿名命名空间（放在 hash32 之后）
// VMD ColorID 式固定色板：相邻下标色相拉开，避免 hash 撞色
constexpr glm::vec3 kChainPalette[12] = {
    {0.00f, 0.00f, 1.00f}, {1.00f, 0.00f, 0.00f}, {0.50f, 0.50f, 0.50f},
    {1.00f, 0.70f, 0.00f}, {0.00f, 0.75f, 0.00f}, {0.55f, 0.20f, 0.80f},
    {0.00f, 0.75f, 0.75f}, {0.85f, 0.40f, 0.60f}, {0.55f, 0.35f, 0.15f},
    {0.90f, 0.90f, 0.20f}, {0.30f, 0.45f, 0.85f}, {0.80f, 0.55f, 0.20f},
};
// 0..1 的稳定扰动（同一链内按分子/残基区分，跨帧不变）
inline float jitter01(uint32_t seed) { return float(hash32(seed) % 1024) / 1023.0f; }

case ColorScheme::Chain: {
    const auto& a = st.atoms[localIdx];
    const glm::vec3 base = kChainPalette[uint32_t(a.chainId) % 12u];
    // 链内 ±6% 亮度扰动：既有"同链同色系"的家族感，又能分辨单个分子/残基
    const float j = jitter01(ctx.instanceIndex * 2654435761u + uint32_t(a.residueId + 1));
    return glm::vec4(glm::clamp(base * (0.94f + 0.12f * j), 0.0f, 1.0f), 1.0f);
}
case ColorScheme::Residue: {   // 逐 resid（对齐 VMD Residue）：按实例+残基区分
    const auto& a = st.atoms[localIdx];
    const float h = float(hash32(ctx.instanceIndex * 131u + uint32_t(a.residueId + 1)) % 3600) / 3600.0f;
    return glm::vec4(hslToRgb(h, 0.42f, 0.60f), 1.0f);
}
```
- 改动点：`representer.cpp:126-139`（约 20 行）；**不动** `gro_parser.cpp`（链名语义保留给选择语言 `chain` 用）。
- 单测影响：现有 176 例**无配色断言**（`grep ColorScheme tests/` 仅 2 处，均只赋值）→ 不会破坏；
  新增 `tests/engine/rep/color_scheme_tests.cpp`（新文件，零冲突）。
- 性能：无（每原子仍是一次哈希；`Residue` 多了 1 次乘法）。
- 兼容：`.mlscene` 键不变（`"chain"`/`"residue"`），旧场景重开**外观会变**（预期）。
- 风险：低。副作用是 2042 个水分子出现 12 级亮度带 —— 但比"整片一色"好，且可在方案 B 里让线框默认用 Name 规避。

### 方案 B（推荐，1 行级）：线框默认配色 = VMD Name，并补 G1/G2
```cpp
// representation.h（在 ColorScheme 定义之后）
inline ColorScheme defaultColorSchemeFor(RepresentationType t) noexcept {
    // VMD 的默认着色是 Name；线框/点这类"细结构"最吃亏的就是 CPK 的白 H
    return t == RepresentationType::Wireframe ? ColorScheme::AtomName
                                              : ColorScheme::ElementCPK;
}
// representation_controller.cpp:92 之后（沿用"仅在从别的样式切过来时给默认"的既有哲学）
if (rep.colorScheme == engine::ColorScheme::ElementCPK) {
    rep.colorScheme = engine::defaultColorSchemeFor(type);
}
// representer.cpp:42 补一行（修 G1）
case ColorScheme::AtomName:    return "按原子名 (VMD)";
// reps_tool.cpp:375-377（修 G2）：补第 6 项，并把下标夹紧
static const char* colorNames[] = {tr("元素 CPK"), tr("按链"), tr("按残基"),
                                   tr("按 BFactor"), tr("统一色"), tr("按原子名 (VMD)")};
int colorIdx = std::clamp<int>(static_cast<int>(edit.colorScheme), 0, IM_ARRAYSIZE(colorNames) - 1);
```
- 单测影响：`tests/engine/rep/render_mode_tests.cpp:195-214` 有"中文名不得为未知"的穷举防线，但只覆盖
  `RepresentationType`；建议**顺手加一个同构的 ColorScheme 穷举用例**（6 个值都非"未知"），正好把 G1 钉死。
- 兼容：只影响**新建/切样式**的表示；已存 `.mlscene` 显式带 `colorScheme`，不受影响。
- 风险：低。唯一争议是"默认被改了"——需要在 `docs/04` 记一笔，并给用户一个"恢复 CPK"的入口（下拉已存在）。
- 遗留 G6：白底 H 仍偏白。建议 AtomName 的 H 用 `(0.80,0.82,0.86)`（冷灰，与白底有 20 级差），
  在 `representer.cpp:158-160` 一处常量即可；**不要**为此改 `line.frag`（加了描边会与球/棍不一致）。

### 方案 C（单独排一轮）：调色板进线性空间 + VMD 参数对齐
```cpp
// representer.cpp 顶部；元素表/HSL/AtomName 的输出统一过一遍
constexpr glm::vec3 srgbToLinear(const glm::vec3& c) {
    auto f = [](float v) { return v <= 0.04045f ? v / 12.92f
                                                : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return glm::vec3(f(c.r), f(c.g), f(c.b));
}
```
- 为什么单独一轮：**所有现有截图、`docs/figs_v2/`、冒烟阈值都建立在"偏淡"的当前观感上**，
  一次性改会让 15 张插图全部要重出。
- 建议同轮把 `cmd.h:61` 的「sRGB 预计算」注释改成「线性值（附件 _SRGB，硬件编码）」，并把
  `element.cpp` 表头注释写明"表中数值是 sRGB 显示值"。
- 单测影响：需要新增颜色断言（当前无），并复查 `render_mode_tests` 里凡比较颜色/材质的地方。
- 风险：中（观感变化大）；收益：**这是"颜色跟 VMD 不一样"的主因**，不做则永远差一档。

---

## 4. VMD → 本项目参数映射表

VMD 侧数据源：`C:\Program Files\VMD\scripts\vmd\materials.dat`（权威表，字段序
`Ambient Diffuse Specular Shininess Mirror Opacity [Outline OutlineWidth]`）。
VMD 的 `Shininess` 是 0–1 归一值，内部 `glMaterialf(GL_SHININESS, shininess*128)`；
本引擎 `shininess` 是 Phong 指数 → **换算 `≈ VMD×128`**。

| VMD 材质 | VMD (A/D/S/Shin×128/Opacity) | 本项目对应 | 现值 | 建议（VMD 观感新预设） |
|---|---|---|---|---|
| `Opaque` | 0.00 / 0.65 / 0.50 / 68 / 1.00 | `kMaterialPresets[0]`（`render_parameter.h:167`） | 0.60/0.90/0.30/32/1.0 | 0.10/0.80/0.45/**64**/1.0 |
| `Transparent` | 0.00 / 0.65 / 0.50 / 68 / **0.30** | `[1]`（`:168`） | 0.30/0.80/0.40/48/0.35 | 0.08/0.72/0.45/64/**0.30** |
| `AOShiny` | 0.00 / 0.85 / 0.20 / 68 / 1.00 | `[17]`（`:184`） | 0.70/0.90/0.85/128 | 0.15/0.88/0.25/64 |
| `AOChalky` | 0.00 / 0.85 / 0.00 / 68 / 1.00 | `[18]`（`:185`） | 0.80/0.94/0.05/8 | 0.20/0.90/0.02/24 |
| `Edgy`（outline 0.62/0.94） | 0.00 / 0.66 / 0.00 / 96 | `[13]`（`:180`） | 0.10/0.80/0.55/32 | 0.02/0.78/0.02/96 |
| `Goodsell`（outline 4.00） | 0.52 / 1.00 / 0.00 / 0 / 1.00 | `[16]`（`:183`） | 0.75/0.92/0.10/16 | 0.52/1.00/0.00/8 |

其余（场景级）映射，参照 `_test/vmd/ref_style.tcl` 逐条对应：

| VMD 命令（ref_style.tcl） | 本项目参数 | 建议值 |
|---|---|---|
| `display background white` / `color Display Background white` | 导出 `--bg white` | 白底 `(1,1,1)`，雾色须同步（`descriptors.cpp:47` 已按线性清屏色处理） |
| `display ambientocclusion on` | 无 AO 通道（`render_parameter.h:145` 已如实声明） | 用 `RenderStyle{wrap 0.50, rim 0.30, outline 0.15, depthCue 0.0}` 近似 |
| `display shadows off` | `LightParams::shadowsEnabled`（`:51`） | `false` |
| `display depthcue off` | `RenderStyle::depthCue`（`:95`） | `0.0`（= `Publication` 预设，`:112`） |
| VMD 无半球光，环境光靠材质 ambient | `LightParams`（`:31-41`） | 做 VMD 预设时 `ambientIntensity 0.20`、`ambientColor (1,1,1)`、`dirLightIntensity 0.80`；默认不动 |
| `mol modcolor 0 0 Name` | `ColorScheme::AtomName` | ✓ 已实现（`representer.cpp:145`），需配色进线性空间（方案 C） |
| QuickSurf `1.0 1.0 0.5` + `Transparent` | `radiusScale/surfGridSpacing/surfIsovalue` + opacity | `1.0 / 1.0 / 0.5`，opacity `0.30` |
| Surf `1.4` + `Opaque` | `RepresentationType::Surface` + `systemSurface` | probe 1.4 → `radiusScale 1.4`，`systemSurface=true` |

坑（已确认，写进验证脚本）：`render TachyonInternal xxx.png` 会**回落 BMP**（文件头 `42 4D`），
`_test/vmd/vmd_ref_*.png` 就是 BMP；两张字节数完全相同是因为同名覆盖。新脚本一律输出 `.bmp` 再转真 PNG。

---

## 5. 验证方案

### 5.1 单测（现有基线 176 例 / 193,708 断言，不允许下降）
| 用例 | 文件 | 断言要点 |
|---|---|---|
| ColorScheme 穷举中文名 | `tests/engine/rep/render_mode_tests.cpp`（改现有文件，+8 行） | 6 个值 `colorSchemeZh != "未知"`（钉死 G1） |
| Chain：同链同色 / 跨链异色 | 新建 `tests/engine/rep/color_scheme_tests.cpp` | `colorForAtom` 同 `chainId` 相等、不同 `chainId` 不等；≥4 个链两两距离 > 0.2 |
| Residue：同残基不同实例不同色 | 同上 | 复用 `render_mode_tests.cpp:243` 的 5 残基夹具思路 |
| 半键拆分 | `render_mode_tests.cpp`（新增 1 例） | `countCmd(LineInst) == 2 * bonds.size()`；两段中点相接 |
| AtomName 色值 | 新建文件 | 6 种元素 → 期望向量（方案 C 后用 linear 期望值） |
| 持久化 | `tests/engine/serialize/scene_bridge_tests.cpp`（+1 例） | `colorScheme="name"` 往返一致 |

> 不要改 `render_mode_tests.cpp:198` 的 `REQUIRE(count == 14)`（表示类型数本轮没变）。

### 5.2 复现数据
- `data/md.gro` —— 必用。2042 分子 / 6452 原子，7 个伪链、SOL 94.5%：**G4/G6 的唯一有效复现集**。
- `data/EPI.gro` —— 单分子，验证"按链"在单链体系下不至于花屏。
- `data/EPI.pdb` / `data/glycine.pdb` —— 有真实 chainID，验证 R2 的"设计语义"没被方案 A 破坏。

### 5.3 并排对比图产出
```bat
:: 1) 自家出图（Release CLI；--style publication 已含白底+无景深淡出）
build\src\cli\Release\moleculens-cli.exe render data\md.gro ^
  --rep "wireframe:all:name" --bg white --style publication ^
  --size 900 --height 700 --out _test\vmd\our_wireframe_name.png
:: 2) VMD 参考（改脚本输出 .bmp，避免 Tachyon 回落）
"C:\Program Files\VMD\vmd.exe" -dispdev text -e _test\vmd\ref_line.tcl
:: 3) 拼图：PowerShell + System.Drawing 左右各 900×700，中间 10px 分隔
```
证据目录：`_test/vmd/`（沿用，勿覆盖已有 `*_conv.png`）。

### 5.4 验收标准（可量化）
1. `our_wireframe_name.png` 非白像素的**量化颜色数 ≥ 4**（当前 2），且单一颜色占比 **< 60%**（当前 93.7%）。
2. 同视角下 O/H/C 三类半键都可辨：`rgb(255,64,64)` 类红像素占比从 93.7% 降到 ≤ 45%，
   且出现 ≥ 5% 的冷色系像素（N 蓝 / C 青）。
3. 与 VMD 同名配色取色差：同一元素在两张图**最饱和像素**上的 sRGB 距离 `ΔE76 < 12`
   （方案 C 未做前放宽到 `< 25`，并在 `docs/04` 注明这是线性化缺口）。
4. `build\tests\Debug\moleculens_tests.exe` ≥ 176 例全过、断言数 ≥ 193,708。
5. 白底 H 可见性：与改动前同视角对比，非白像素数**增加 ≥ 20%**（H 半键从"无"变成"有"）。

---

## 6. 风险与"不要做的事"

- ❌ 不要把 `LineInst` 从 48B 扩成 64B（双端颜色）：`cmd.h` 命令协议有 `static_assert(48)`，
  且 `decode.cpp`/`draw_geometry.cpp`/测试都按 48B 走；半键拆分已经能到同样效果，**收益为负**。
- ❌ 不要为了"H 可见"给 `line.frag` 加描边/压暗：会和球/棍（走 `material_shading.glsl`）观感割裂。
- ⚠ 方案 C 会推翻现有 15 张插图与冒烟阈值，必须与"重出 docs/04 DOCX + figs_v2"同一轮做。
- ⚠ `render_parameter.h:143-149` 已明确承认"材质数值是按本引擎调校的近似值，不是 VMD 原表"——
  本文 §4 给的是**新增 VMD 观感预设**的建议值，不要把现有 23 项的数值直接替换（会动所有旧场景外观）。
- 📌 交接文档需要更正的两处：①`ColorScheme` 现在有 **6** 个值（`representation.h:35-43`），
  不是 5 个；②"一个链一个颜色就是 `ColorScheme::Chain` 的现有实现"只说对了一半——
  md.gro 上的直接原因是 GRO 把**残基名**当链（`gro_parser.cpp:186-196`），加上旧线框的**单端配色**。
