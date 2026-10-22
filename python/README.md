# MolecuLens Python 接口

`python/` 提供两层能力，覆盖「读结构 → 选择 → 出图 → 动画 → 导出」全流程：

* **数据层**（pybind11 绑定，`moleculens.load` / `Structure`）：解析
  PDB / GRO / SDF / cube，暴露原子、键、坐标、统计与选择语言。只链接
  `moleculens_entity`，不引入 Vulkan / GLFW，无图形环境也能导入。
* **绘图层**（`moleculens.render` / `render_animation` / `convert`）：
  拼装参数后调用同仓库的 `moleculens-cli`。GUI、CLI、Python 共用同一套
  C++ 渲染实现，Python 侧不复制材质/相机/表面参数表。

## 目录结构

```text
python/
├─ CMakeLists.txt             # pybind11 模块 _native + 包文件/DLL 拷贝 + CTest
├─ module/
│  └─ pybind_module.cpp       # C++ 绑定（entity / io / selection）
├─ moleculens/
│  ├─ __init__.py             # 公共包（load / info / render / convert）
│  └─ _cli.py                 # CLI 封装（定位可执行文件、参数拼装、错误）
├─ examples/
│  └─ render_gallery.py       # 七步绘图全流程示例（单帧/透明/轨迹/ESP/光追/动画/导出）
├─ tests/
│  ├─ conftest.py             # 构建产物、DLL 搜索路径、测试数据
│  ├─ test_api.py             # 包接口、元素表、容器协议、路径类型
│  ├─ test_parse_formats.py   # PDB / GRO / SDF / cube / 多帧统计
│  ├─ test_selection.py       # 选择语言与 Python 独立实现逐项比对
│  ├─ test_errors.py          # 异常类型、畸形文件、Unicode 路径
│  └─ test_render_pipeline.py # 真实 CLI 绘图全流程（PNG/GIF/OBJ 产物校验）
├─ pytest.ini
└─ README.md
```

## 构建

前置条件：CMake ≥ 3.21、MSVC 2022（或其他 C++17 编译器）、Python 3.9+。
`pybind11` 是 header-only 依赖，推荐直接安装到目标解释器：

```powershell
python -m pip install pybind11 pytest pillow
```

启用绑定，并构建扩展与 CLI（绘图测试需要 CLI）：

```powershell
cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON
cmake --build build --config Release --target _native moleculens-cli
```

产物在 `build/python/moleculens/`：

```text
build/python/moleculens/
├─ __init__.py / _cli.py        # 从 python/moleculens/ 拷贝
├─ _native.cp39-win_amd64.pyd   # 原生扩展
└─ spdlog.dll / fmt.dll ...     # POST_BUILD 自动拷贝的运行期 DLL
```

> `MOLECULENS_BUILD_PYTHON=ON` 会一并配置整个工程；若只想得到 Python 模块，
> 仍可直接构建 `_native` 目标（它只依赖 `moleculens_entity`）。
>
> Windows 上 CPython 调试库通常未安装，且调试 CRT 的扩展不应混入 Release，
> 因此 **只有 Release 写入固定的 `build/python/`**；Debug 等配置输出到
> `build/python-debug/`、`build/python-relwithdebinfo/`。测试请使用 Release。

CMake 查找 pybind11 的顺序：`find_package(pybind11 CONFIG)` → 若未找到，执行
`<Python3_EXECUTABLE> -m pybind11 --cmakedir` 回退到 pip 安装的配置。

## 测试

```powershell
$env:PYTHONPATH = "$PWD\build\python"
python -m pytest python\tests -q
```

预期结果：`54 passed`（46 个数据层 + 8 个绘图全流程）。如果构建目录不是
`build`，把 `MOLECULENS_PYTHON_DIR` 指到包含 `moleculens/` 的目录；
`MOLECULENS_CLI` 可显式指定 CLI 路径（默认自动查找
`build/src/cli/<Release|RelWithDebInfo|Debug>/moleculens-cli.exe`）。

绘图测试在以下情况自动跳过：CLI 未构建、Pillow 未安装、ESP/轨迹夹具缺失。

也可以走仓库统一的 CTest 入口（`MOLECULENS_BUILD_TESTS=ON` 时注册）：

```powershell
ctest --test-dir build -C Release -R moleculens_python --output-on-failure
```

## 快速上手：数据层

```python
import moleculens

mol = moleculens.load("data/EPI.pdb")

len(mol), mol.atom_count, mol.bond_count      # 35, 35, 37
mol.atoms[0].element                          # 'O'
mol.atoms[0].residue_name                     # 'EP'
mol.atoms[0].position                         # (0.455, -0.756, -0.112) Å

# 选择语言：返回全局原子下标，与渲染器/场景坐标段同一套编号
carbons = mol.select("element C")
polar   = mol.select("element O or element N")
near_o  = mol.select("within 1.9 of element O")

# 需要掩码或表达式元信息时用 Selection 对象
sel = mol.selection("element C and not name C07")
sel.count, sel.indices, sum(sel.mask)

# 统计口径与 `moleculens-cli info` 一致（按实例累加）
mol.summary()["composition"]        # {'C': 15, 'H': 14, 'O': 6}
mol.summary()["warnings"]           # 解析警告（行号 + 消息）

moleculens.element_info("C").mass   # 12.011
moleculens.element_info(8).name     # 'Oxygen'
```

## 绘图全流程

参数名与 `moleculens-cli render --help` 一一对应（连字符换下划线）；
无法预见的参数可用 `extra_args=["--esp-specular", "0.3"]` 透传。

```python
import moleculens

# 1) 出版风格单帧：PDB → 球棍 → 白底 PNG
moleculens.render(
    "data/EPI.pdb", "figs/epi.png",
    rep="ballstick", style="publication", size=(1280, 720),
)

# 2) 透明背景 + 链配色；depth=16 导出 16-bit PNG
moleculens.render(
    "data/EPI.pdb", "figs/epi_transparent.png",
    rep="wireframe", coloring="chain",
    background="transparent", depth=16, size=(800, 600),
)

# 3) 轨迹指定帧（md.gro + md.xtc）
moleculens.render(
    "data/md.gro", "figs/frame25.png",
    rep="wireframe", trajectory="data/md.xtc", frame=25,
    style="publication", size=(800, 600),
)

# 4) ESP 映射表面（密度 cube 提供几何，势 cube 提供颜色）
moleculens.render(
    "datasets/esp/glycine/glycine_density.cube", "figs/esp.png",
    esp="datasets/esp/glycine/glycine_esp.cube",
    cmap="coolwarm", vmin=-40, vmax=40, size=(800, 600),
)

# 5) CPU 光线追踪（--rt-backend gpu 需要硬件光追设备）
moleculens.render(
    "data/EPI.pdb", "figs/epi_rt.png",
    rep="ballstick", ray_trace=1, rt_backend="cpu", size=(640, 480),
)

# 6) 轨迹动画：扩展名决定格式（.gif / .apng / .mp4 / .webm）
moleculens.render_animation(
    "data/md.gro", "figs/trajectory.gif",
    trajectory="data/md.xtc", frame_range="0:30", fps=15,
    rep="wireframe", size=(640, 480),
)

# 7) 几何导出（.obj / .stl / .pov / .mlscene；OBJ 同时写 .mtl）
moleculens.convert("data/EPI.pdb", "figs/epi.obj", rep="ballstick")
```

`moleculens.render(...)` 返回输出路径；失败（非零退出码/超时）抛
`moleculens.CliError`，异常对象保留 `command` / `returncode` / `stdout` /
`stderr`，可直接复现。CLI 查找顺序：显式 `executable=` → 环境变量
`MOLECULENS_CLI` → `PATH` → 仓库 `build/src/cli/<Config>/`。

一键跑完七步并打印耗时：

```powershell
$env:PYTHONPATH = "$PWD\build\python"
python python\examples\render_gallery.py --out-dir .tmp\render_gallery
```

## 语义约定

| 项 | 约定 |
|---|---|
| 原子下标 | 全局下标：`instances` 顺序 × 结构原子顺序，与 `SceneStructure::coordsSoA()` 一致 |
| `Structure` | 加载时刻的**只读快照**；后续 C++ 场景变化不会回写 |
| `occupancy` | 0–100（与选择语言 `occupancy` 关键字一致，不是 0–1） |
| `bfactor` | Å²（内核以 ×100 整数存储，绑定层已还原） |
| `residue_id` / `chain_id` | `SceneStructure` 的残基/链**表下标**；`select("resid ...")` 使用同一编号 |
| 坐标 | 统一为 Å；GRO 的 nm 坐标在解析时换算 |
| 测试口径 | `atom_count` / `bond_count` 按实例累加（`md.gro` = 6452 原子，而非首个水分子的 3 个） |
| 数据层错误 | 文件缺失 → `FileNotFoundError`；目录 → `IsADirectoryError`；解析失败 → `MoleculensError`（消息前缀为错误码，如 `FormatError:`）；非法选择表达式 → `ValueError` |
| 绘图错误 | CLI 非零退出码/超时 → `CliError`（含完整命令行与 stderr） |

## 能力边界

已覆盖：

- PDB / ENT、GRO、SDF / MOL、Gaussian cube / cub 解析（与 CLI 同一注册表）；
- 原子字段（元素、原子名、残基/链、occupancy、B-factor、坐标、CPK 色）；
- 键表（全局下标 + 键级，0 表示芳香）；
- 选择语言：属性比较、`AND` / `OR` / `NOT`、括号、`within`、`same residue`；
- 单帧渲染：表示/配色/材质/风格/相机/背景/剖切/SSAA/16-bit/光追/ESP；
- 轨迹帧渲染、GIF/APNG/MP4/WebM 动画、OBJ/STL/POV/mlscene 导出；
- 解析警告与统计摘要。

暂未覆盖（按需扩展）：

- 在 Python 进程内直接持有 Vulkan 设备/实时视口（当前一律离屏出图）；
- 轨迹逐帧坐标的 Python 侧读取（`frame_count` / `dt` 已暴露，帧数据未绑定）；
- 场景编辑（增删实例、变换、表示与配色）；
- `Structure` 的切片与原地修改（`atoms` / `bonds` 为普通 list，可直接切片）。
