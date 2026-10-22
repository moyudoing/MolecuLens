# 为 MolecuLens 贡献

感谢愿意花时间参与。这份文档写清楚三件事：怎么把环境跑起来、这个仓库坚守哪些约定、以及一次改动要过哪些检查。

**[English](CONTRIBUTING.md)**

## 可以怎么贡献

* **缺陷报告** —— 崩溃、几何不对、颜色不对、CLI / 导出器出错、格式读不进来。
  用 [缺陷报告表单](.github/ISSUE_TEMPLATE/bug_report.yml)，附上能复现问题的最小结构文件或轨迹。
* **功能建议** —— 比修 bug 大的改动，请先开 [功能建议](.github/ISSUE_TEMPLATE/feature_request.yml)，
  把设计聊清楚再动代码。
* **文档与插图** —— 长文档是中文的；纠错、英文翻译、更好的插图都欢迎。
* **代码** —— 从 issue 出发，保持改动可评审（见下文「Pull Request」）。

## 动手之前

1. 先搜 issue、[ROADMAP](docs/_internal/ROADMAP-渲染与DFT视图.md) 与
   [ISSUES](docs/_internal/ISSUES-已知问题登记.md)：不少已知问题已经有方案了。
2. 非小改动请先开 issue 对齐方案。碰命令流或渲染管线的改动，写清设计再评审会快很多。
3. 读一遍 [REMAINING_WORK.md](REMAINING_WORK.md)：里面记着构建/环境约定，以及「不要重做」的清单。

## 开发环境

依赖、版本与平台说明见 [DEPENDENCIES.md](DEPENDENCIES.md)。简版：

* CMake ≥ 3.21、C++20 编译器（MSVC 2022 / GCC 11+ / Clang 14+）、[vcpkg](https://github.com/microsoft/vcpkg)、Vulkan SDK
* 可选：`ffmpeg` 在 `PATH` 上（或设置 `MOLECULENS_FFMPEG`），用于 MP4 / WebM / GIF 导出

```bash
git clone https://github.com/moyudoing/MolecuLens.git
cd MolecuLens
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug -j

./build/src/app/Debug/moleculens_app data/EPI.pdb     # GUI
./build/src/cli/Debug/moleculens-cli info data/EPI.pdb
./build/tests/Debug/moleculens_tests                  # 测试
```

开发期用 `Debug`，提 PR 前再验证一遍 `Release` —— 性能相关的结论必须来自 `Release`。

## 代码约定

* **命名** —— 类 `PascalCase`、方法 `snake_case`、成员 `lowercase_`、命名空间 `moleculens`。
* **依赖方向** —— `moleculens_entity ← moleculens_engine ← app / cli`。`entity` 不许包含 engine / app 的头文件。
* **目录粒度** —— 每个目录最多三个 `.cpp/.h` 对，由 `python cmake/check_tree.py` 检查；
  文件涨了就拆子目录，不要往里堆。
* **协议头** —— `src/engine/cmd/cmd.h`、`src/engine/persist/mlscene/mlscene.h` 必须能独立编译
  （不依赖工程内其它头文件）。
* **命令流** —— 任何让缓存表示失效的改动都要 `bumpSerial()`；渲染器靠 serial 决定重传什么。
* **不要悄悄改默认值** —— 默认参数变了，就在 PR 描述和 `docs/_internal/` 里写明。

## 渲染改动必须逐位一致

这是本项目给自己定的硬标准，也是评审时查得最严的一条：

1. 改之前先出一张基线图并记下 md5：

   ```bash
   ./build/src/cli/Release/moleculens-cli render data/EPI.pdb --rep ballstick \
       --size 960 --height 540 --out before.png
   md5sum before.png     # Windows: certutil -hashfile before.png MD5
   ```

2. 改完用同一条命令再出一张对比。几何等价的改动（BVH vs 线性、多线程 vs 标量 QuickSurf、网格缓存）
   **必须**得到完全相同的 PNG。
3. 如果画面本来就该变，请在 PR 里明确说明，重新生成 `docs/figs_v2/` 里受影响的插图，并写进变更记录。

测试里已经用 PNG md5 锁住了光追与 QuickSurf 两条路径；新增路径时请照这个模式补上。

## 测试

* 推送前跑 `moleculens_tests`（或 `ctest --test-dir build`）。当前基线是
  **187 个用例 / 193,781 条断言**，只增不减。
* 新行为要有测试。新增表示、导出器、格式读取、调度规则都能在现有测试里找到模板。
* 修 bug 请附上「本来能抓住这个 bug」的回归测试。

## 提交信息

用 Conventional Commits，scope 写层次或模块：

```text
feat(engine): 共享单位球网格 + 逐实例变换
fix(app): 视口变窄时时间轴仍然可见
docs(readme): 补命令行示例
test(engine): BVH 与线性遍历逐像素一致
refactor(cmd): 把命令重放与网格重建拆开
build(cmake): 升级 vcpkg baseline
```

几条经验：

* 一个提交只做一件事；纯格式化改动单独提交。
* 看 diff 说不清「为什么」时，在正文里补上原因。中文英文都可以。
* 不要提交构建产物、`build_*/`、`_test/`、`.monitor/`，以及并非本次特意重建的插图。

## Pull Request

1. 从 `master` 切分支，名字对应改动（`feat/shared-sphere-mesh`、`fix/timeline-layout`）。
2. 保持可评审。重构和行为改动混在一个 PR 里，很难用逐位一致去验证——请拆开。
3. 按 [PR 模板](.github/PULL_REQUEST_TEMPLATE.md) 填写：跑了什么命令、测试数是多少。
4. 明确写清渲染输出有没有变；没变就贴改前改后的 md5。
5. 正在评审的分支不要 force-push，追加提交即可，压不压交给维护者决定。
6. CI（.github/workflows/quality.yml）检查文档相对链接、第三方产品名与目录粒度；C++ 测试仍在本地跑，请先自己跑通，别让评审者替你发现编译错误。

## 文档

* 行为变了就更新 `docs/` 下对应文档、`docs/_internal/` 里的变更记录；用户可见的变化还要同步 README 功能表。
* 长文档以中文为准；README 是中英双语（`README.md` 英文、`README.zh-CN.md` 中文），动一处请两边同步。
* `docs/figs_v2/` 里的插图应当能由命令行复现，新增插图时在 PR 里贴出命令；`docs/06-开源数据集与演示.md` 的图库就是这样生成的。
* `datasets/` 新增文件必须可再分发（公有领域、CC0/CC-BY、MIT/BSD）；禁止 GPL、NC、ND 或许可不明数据，并在 `datasets/README.md` 记录来源、许可与 SHA-256。

## 许可证与签署

项目采用 **Apache-2.0**。提交 PR 即表示你同意以同样许可授权你的贡献（DCO 风格），请签署提交：

```bash
git commit -s -m "fix(engine): ..."
```

签署表示代码由你编写，或你有权提交。不要粘贴许可证不兼容项目的代码；从论文或其它代码库借鉴算法时，
在 PR 里注明来源与其许可。

仓库里有两个文件**故意**不在 Apache-2.0 授权范围内，改动时不要破坏这一点：
Noto Sans SC 字体子集（SIL OFL 1.1，`data/font/OFL.txt`）与 MDAnalysis 的 XTC 测试夹具
（GPL-2.0+，见 `THIRD_PARTY_NOTICES.md`）。

## 行为准则

讨论保持技术性和礼貌。设计上有分歧很正常；人身攻击、骚扰、以及「你自己看着改吧」式的敷衍不接受。
