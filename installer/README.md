# MolecuLens 安装包（Windows x64）

本目录是发布打包的**唯一入口**：CMake 安装规则在 `cmake/packaging.cmake`，安装程序脚本是本目录的
`moleculens.nsi`（NSIS 3 / Unicode），构建与打包流程由 `tools/package_release.ps1` 驱动。

## 产物

| 文件 | 说明 |
|---|---|
| `dist/MolecuLens-<版本>-win64-Setup.exe` | 安装程序（NSIS，约 3.4 MB，内含 LZMA 压缩的全部运行文件） |
| `dist/MolecuLens-<版本>-win64-portable.zip` | 免安装版（解压即用，含同样的 exe/DLL/字体/示例数据） |
| `dist/SHA256SUMS.txt` | 两个产物的 SHA-256，供发布页与下载校验 |

## 一条命令重建

```powershell
# 任意 PowerShell 窗口即可，不需要先进入 VS 开发者环境（脚本会自行导入 vcvars64）
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

常用参数：

```powershell
- Version 2.20.0     # 发布版本号（同时写入安装包文件名与注册表 DisplayVersion）
- SkipBuild          # 复用已有 build_pkg 产物，只重新打包
- SkipZip            # 只出安装程序
- SkipInstaller      # 只出免安装 ZIP
- BuildDir build_pkg # 构建目录
- OutDir dist        # 产物目录
```

脚本依次执行：

1. 定位并导入 MSVC 环境（`vcvars64.bat` → `INCLUDE`/`LIB`，Ninja 命令行编译的必要条件）；
2. 用 Ninja 配置 `Release`（默认复用 `build/vcpkg_installed`，不会重复编译 vcpkg 依赖）；
3. 编译 `moleculens_app` 与 `moleculens-cli`；
4. `cmake --install` 生成 `out/stage`（exe + 运行库 + 字体 + 示例数据 + 许可）；
5. 从 `docs/figs_v2/logo.png` 生成安装器品牌图（欢迎页 164×314、页眉 150×57）；
6. 调用 `makensis` 编译安装程序；
7. `cpack -G ZIP` 打免安装包；
8. 计算 SHA-256 并打印产物清单。

## 安装程序的约定

* **按用户安装，不需要管理员权限**：默认目录 `%LOCALAPPDATA%\Programs\MolecuLens`。
  程序运行时会往安装目录写 `logs/` 和 `imgui.ini`，按用户安装天然可写，避免
  Program Files 的权限问题；卸载条目写在 `HKCU\...\Uninstall\MolecuLens`，
  控制面板「应用和功能」可见。
* **组件**：主程序（必需）/ 命令行工具 / 示例数据 / 开始菜单快捷方式 / 桌面快捷方式。
* **静默安装**：`Setup.exe /S [/D=目标目录]`（`/D` 必须是最后一个参数且不用引号）；
  静默卸载：`"<安装目录>\uninstall.exe" /S`。
* **前置检查**：非 64 位系统直接拒绝；缺少 `vulkan-1.dll` 只提示不阻断；旧版本正在运行时
  询问是否关闭（静默模式直接关闭），避免文件占用导致解压失败。
* **卸载保留用户偏好**：`%APPDATA%\MolecuLens\settings.json` 不删除，重装后主题/布局照旧。

## 运行库策略

* `glfw3.dll`、`spdlog.dll`、`fmt.dll` 来自 vcpkg（`x64-windows` 动态三元组），由
  `cmake/packaging.cmake` 显式安装；imgui / volk / lodepng / glm 是静态库，不入包。
* MSVC CRT 采用 **app-local** 部署（`concrt140/msvcp140*/vccorlib140/vcruntime140*`），
  目标机无需预装 VC++ 2015-2022 Redistributable；路径来自 `VCToolsRedistDir`
  或从 `cl.exe` 位置推导，取版本号最大的 `Microsoft.VC14*.CRT`。
* Vulkan 加载器（`vulkan-1.dll`）**不打包**：它随显卡驱动安装，由 volk 运行时动态加载。

## 依赖工具

| 工具 | 用途 | 缺省来源 |
|---|---|---|
| CMake ≥ 3.21 | 配置/安装/CPack | `E:\CMake\bin\cmake.exe` 或 PATH |
| Ninja | 构建 | CLion 自带或 PATH |
| MSVC 2022 (v143) | 编译 | `vswhere` 自动定位 |
| vcpkg | 依赖（manifest） | `F:\vcpkg` 或 `VCPKG_ROOT` |
| NSIS 3 + `nsProcessW.dll` | 编译安装程序 | 本机任意 NSIS 3 发行版，脚本按 `tools\nsis\makensis.exe`、`.tmp\pkg\nsis\nsis-bundle\windows\makensis.exe`、PATH 顺序查找 |

`makensis` 若不是标准安装（例如直接解压的 zip），脚本会把 `NSISDIR` 指向 exe 所在目录，
因为这类发行版的 `Stubs/`、`Contrib/`、`Plugins/` 与 `makensis.exe` 同级。

## 维护注意

* `installer/moleculens.nsi` 与 `tools/package_release.ps1` 必须保存为 **UTF-8 with BOM**：
  Windows PowerShell 5.1 会把无 BOM 的 UTF-8 当作 ANSI（GBK）解析，中文字符串会破坏
  语法；NSIS 3 同样依赖 BOM 识别 Unicode 脚本。
* 版本号只改一处：`-Version`（或 `cmake/packaging.cmake` 的 `MOLECULENS_PRODUCT_VERSION`
  默认值）。`VIProductVersion` 由 `${VERSION}.0` 生成，因此版本号必须是 `X.Y.Z`。
* 新增依赖若提供 DLL，需要在 `cmake/packaging.cmake` 第 5 节补上，或在 vcpkg 三元组中
  改为静态库，否则安装后会出现 `0xc0000135`（找不到 DLL）。
