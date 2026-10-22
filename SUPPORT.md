# 获取帮助 / Support

MolecuLens（分子视镜）目前由维护者利用业余时间开发，**没有商业支持**。提问前先花五分钟
自查，通常能当场解决；确实要问，请走下面列出的入口，问题描述越具体，解决得越快。

## 先自查这三步

1. **看文档**——安装、界面、每个模块的操作与全部命令行参数都在《使用说明》里；
   「现象 + 报错原文」多半能在 FAQ 里搜到。
2. **看已知问题**——[ISSUES-已知问题登记](docs/_internal/ISSUES-已知问题登记.md) 与
   [ROADMAP](docs/_internal/ROADMAP-渲染与DFT视图.md) 记录了已确认的缺陷和下一步计划，
   避免重复报告。
3. **确认构建方式**——`Release` 与 `Debug` 的性能差好几倍；渲染相关问题请先确认复现用的
   是哪种构建。依赖版本见 [DEPENDENCIES.md](DEPENDENCIES.md)。

## 文档导航

| 想知道什么 | 看这里 |
| --- | --- |
| 装不上、跑不起来、界面怎么用、CLI 参数 | [使用说明](docs/03-使用说明.md) |
| 有哪些表示 / 材质 / 配色，哪些还不支持 | [功能介绍书](docs/02-功能介绍书.md) |
| 内部怎么分层、命令流与渲染管线怎么走 | [软件架构书](docs/01-软件架构书.md) |
| 真实算例上的性能与导出实测 | [测试报告](docs/04-测试报告.md) |
| DFT 出图：图从哪来、参数怎么配 | [DFT 常用绘图指南](docs/05-DFT常用绘图指南.md) |
| ESP 表面的数据通路与作图配方 | [ESP_SURFACE.md](ESP_SURFACE.md) |
| CMake / vcpkg / Vulkan SDK / ffmpeg 版本 | [DEPENDENCIES.md](DEPENDENCIES.md) |
| 本轮开放工作与"不要重做"清单 | [REMAINING_WORK.md](REMAINING_WORK.md) |
| 每个版本改了什么、为什么 | `docs/_internal/` 下的版本记录 |

## 提问入口

- **缺陷（崩溃、几何/颜色错误、文件读不进来、导出异常）**：用
  [Bug report 表单](.github/ISSUE_TEMPLATE/bug_report.yml) 提 issue。
- **功能建议 / 新的表示、格式、工作流**：用
  [Feature request 表单](.github/ISSUE_TEMPLATE/feature_request.yml)。
- **想直接改代码**：先读 [CONTRIBUTING.md](CONTRIBUTING.md)，按里面的分支与提交约定来。
- **安全问题**：**不要开公开 issue**，按 [SECURITY.md](SECURITY.md) 走私密报告通道。
- **行为准则相关**：见 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

## 一个能被修的问题长什么样

缺了下面任何一项，维护者大概率只能回一句"无法复现"：

| 必填项 | 示例 |
| --- | --- |
| 版本 | v2.13.0，或 `git rev-parse --short HEAD` 的输出 |
| 构建 | `Release` / `Debug`，编译器与版本（MSVC 2022 17.9、GCC 13.2……） |
| 系统 | Windows 11 23H2 / Ubuntu 22.04 / macOS 14 |
| 显卡与 Vulkan | `vulkaninfo --summary` 的前几行，或显卡型号 + 驱动版本 |
| 复现输入 | 能触发问题的最小结构 / 轨迹 / cube 文件（可脱敏、可截断，说明原文件多大） |
| 复现步骤 | 完整命令行，或"打开哪个文件 → 点哪个面板 → 点什么" |
| 实际结果 | 报错原文、崩溃栈、日志片段、截图（渲染问题请附期望效果） |

> 大文件不要直接塞进 issue。先截到能复现的最小体系（例如只保留一个分子或前 10 帧），
> 再贴网盘链接并说明许可是否允许我们用于回归测试。

## 不在这里处理的事

- 商业授权、定制开发、论文署名：请直接联系维护者。
- 「能不能帮我算这个体系的 ESP」：本项目只负责可视化，量化计算请用你已有的计算流程。
- 与第三方驱动或系统组件相关的崩溃：请同时把信息反馈给对应厂商。
