# ESP 映射分子表面（Electrostatic Potential Surface）

参考效果：GXNU MolStudio（微信公众号「Chem311-GXNU」）的 ESP 表面 —— 电子密度等值面
（默认 ρ = 0.001 a.u.，即 vdW 表面）+ 静电势逐顶点连续着色 + 色标条 + 表面极值点数值标注，
半透明表面内部可见球棍模型。

## 快速用法（CLI）

```powershell
# density cube 作为输入结构（.cube 已注册为可打开格式），--esp 指定静电势 cube
# 默认即是参考截图观感（Reference 色标：负电势黄、正电势紫；半透明+球棍）
.\build\src\cli\Release\moleculens-cli.exe render density.cube `
  --esp ESP.cube `
  --size 1600 --height 1200 --scale 3 --bg white --camera front `
  --out esp.png

# 化学惯例配色（红=负电势/富电子，蓝=正电势/缺电子）：
#   --cmap rwb
```

参数：

| 参数 | 说明 |
|---|---|
| `--esp <path>` | 静电势 cube（Multiwfn 的 `totesp.cub` / 高斯 ESP cube）；提供后自动叠加 ESP 表面表示 |
| `--iso <a.u.>` | 密度等值面阈值，默认 0.001（vdW 表面）；越小表面越大 |
| `--vmin/--vmax <kcal/mol>` | 色标范围；默认自动（发散型取 ±99.5 分位 \|v\|） |
| `--no-auto-range` | 关闭自动范围，改用 `--vmin/--vmax` |
| `--cmap <name>` | 色标：`reference`(默认，参考截图「黄→绿→青→蓝→紫」) / `spectral` / `rainbow` / `rwb` / `bwr` / `coolwarm` / `turbo` / `viridis` / `jet` |
| `--invert` | 翻转色标方向 |
| `--esp-opacity <0.05~1>` | 表面不透明度（默认 0.85；调低更通透、调高颜色更饱和） |
| `--esp-diffuse <0.2~1>` | 表面颜色深浅（默认 0.90；调低变深、提高发白） |
| `--esp-ambient <0~1>` | 表面环境光（默认 0.30） |
| `--esp-specular <0~1>` | 表面高光强度（默认 0.15） |
| `--esp-shininess <n>` | 表面高光指数（默认 48） |
| `--repr espsurface` | 只画表面不画球棍；默认球棍 + 表面 |
| `--surf-iso <阈值>` | 表面/快速表面（非 ESP）的等值面阈值，缺省 0.5 |
| `--surf-grid <Å>` | 表面/快速表面的网格间距，缺省 0.5；越小越精细越慢 |
| `--traj <path>` / `--frame <n>` | 用轨迹（DCD/XTC/TRR）第 n 帧的坐标渲染；`-1` 表示末帧 |
| `--light-az <度>` / `--light-el <度>` | 覆盖场景默认光向：方位角 0 = 正对相机 +Z、180 = 背光；仰角 90 = 正上方。用于复现只在特定光向下出现的渲染问题 |
| `--bg transparent` | 真透明背景（清屏 alpha=0，几何 alpha=1），适合论文叠底 |

- 参考截图配色（默认）：`--cmap reference`（负电势黄、正电势紫；低值黄→绿→青→蓝→紫高值）
- 化学惯例配色：`--cmap rwb`（红 = 负电势/富电子，蓝 = 正电势/缺电子）
- 导出 PNG 自带右侧色标条与极值点数值标注（无需后处理）

## 数据准备（Multiwfn）

1. 由 Gaussian 得到 `.fchk`；
2. Multiwfn 依次 `5`（输出 cube）→ `1`（电子密度）→ `3`（网格设置）→ `2`（输出 density.cub），
   再 `5`（输出 cube）→ `12`（静电势）→ 输出 `totesp.cub`；
3. 用 `density.cub` 作为输入结构、`totesp.cub` 作为 `--esp`。

> GUI 中后续支持：打开 density.cube 后在「表示」面板选「ESP 表面」并指定 ESP cube 路径。

## 代码结构

| 位置 | 内容 |
|---|---|
| `src/io/cube/cube_volume.h` | 契约：`entity::CubeVolume`（dims/axes/origin/data/atoms）+ `parseCubeFile` |
| `src/io/cube/cube_parser.{h,cpp}` | Gaussian cube 解析（Bohr→Å，快速扫描），`CubeFileReader` 注册为 `.cube/.cub` |
| `src/engine/style/espsurface/esp_surface.{h,cpp}` | Marching Cubes 等值面 + ESP 三线性采样 + 7 色标 + 极值点检测 + LRU 缓存 + overlay 数据 |
| `src/engine/export/overlay/esp_overlay_raster.h` | 色标条/极值标注 CPU 栅格化（CLI 出图与 GUI 导出共用；TTF 文字 + 描边，5x7 位图回退） |
| `src/engine/export/overlay/text_raster_ttf.{h,cpp}` | TTF 文字光栅化（stb_truetype + OPPO B.ttf，抗锯齿，导出图文字清晰） |
| `src/engine/style/style_registry.*` / `base_style.h` | 新表示 `RepresentationType::EspSurface` 注册 |
| `src/cli/cmd/render/render_cmd.cpp` | `--esp` 等参数与叠加层调用 |
| `src/persist/mlscene/*` + `src/engine/serialize/scene_bridge.cpp` | ESP 参数随 `.mlscene` 持久化（v3 可选键，旧文件兼容） |

单位与约定：

- 长度：cube 文件内为 Bohr，解析后统一 Å；
- ESP 数值：cube 内为 Hartree(a.u.)，显示/色标统一换算 kcal/mol（×627.509474）；
- 密度等值面阈值保持 a.u.（0.001 ≈ vdW 表面）。

## 已知限制 / 后续

- 极值点为引擎自动检测（顶点邻域局部极值 + 1.2 Å 非极大抑制，最多 8 极大 + 8 极小），
  与 Multiwfn `surfanalysis.pdb` 的官方极值可能有差异；后续可加"导入 Multiwfn 极值文件"。
- 半透明表面目前只绘制正面（背面剔除）：不透明通道里已渲染的球棍/背景可以正确透过表面
  显示，避免了双层乱序混合导致的细亮线伪影；真正的玻璃质感（双面 + 排序）可后续做
  OIT/深度剥离。
- ESP 表面目前不参与阴影投射（与 TriMesh 现有阴影策略一致）。
- 开发用合成数据：`_esp/make_cube.py` 可生成甘氨酸的 density.cube / ESP.cube（仅用于
  渲染管线验证，不是真实量化结果）。
