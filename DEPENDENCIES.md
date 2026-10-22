# MolecuLens · 第三方依赖清单

> 规格来源：主方案 §14.2 依赖清单 / M0 工程脚手架 §3.2（vcpkg.json）。
> 本文系统性列出全部第三方依赖的**安装方式、版本要求、环境配置与基本用法**。

---

## 0. 依赖总览

| 依赖 | 用途 | 必需性 | vcpkg 包名 |
|---|---|---|---|
| CMake ≥ 3.21 | 构建系统 | 必需 | —（系统工具） |
| vcpkg | 包管理（manifest 模式） | 必需 | —（系统工具） |
| Vulkan SDK | glslangValidator / 校验层 / vulkan-1.dll | 必需 | —（系统工具） |
| GLFW 3.4 | 窗口/输入（Vulkan 专用） | 必需 | `glfw3` |
| Volk | Vulkan 动态加载（避免链 loader） | 必需 | `volk` |
| Vulkan-Headers | Vulkan 头文件 | 必需 | `vulkan-headers` |
| VMA (vulkan-memory-allocator) | GPU 内存管理 | 必需 | `vulkan-memory-allocator` |
| glm 1.0 | 数学库（vec/mat/quat） | 必需 | `glm` |
| Dear ImGui (docking) | UI 框架 | 必需 | `imgui` + `docking-experimental` |
| spdlog | 日志 | 必需 | `spdlog` |
| nlohmann-json | JSON（settings/.mlscene 元数据） | 必需 | `nlohmann-json` |
| CLI11 | 命令行解析 | 必需（CLI） | `cli11` |
| Catch2 v3 | 单元测试 | 必需（测试） | `catch2` |
| pybind11 | Python 绑定（可选） | 可选 | `pybind11` |
| cppzmq / libzmq | ZeroMQ RPC（可选） | 可选 | `cppzmq`（非 Windows） |
| ffmpeg | MP4/WebM 动画导出（可选） | 可选 | —（外部程序） |

---

## 1. 系统工具

### 1.1 CMake（≥ 3.21，推荐 3.31+）

- **下载**：<https://cmake.org/download/>（Windows 选 `cmake-3.31.x-windows-x86_64.msi`）
- **或包管理器**：`winget install Kitware.CMake` / `choco install cmake` / `apt install cmake`
- **环境**：`cmake --version` 可执行；PATH 含 cmake.exe

### 1.2 vcpkg（manifest 模式）

- **安装**（首次）：
  ```powershell
  git clone https://github.com/microsoft/vcpkg F:\vcpkg
  F:\vcpkg\bootstrap-vcpkg.bat
  ```
- **环境**：设置 `VCPKG_ROOT=F:\vcpkg`（本机已配置于 F:\vcpkg，版本 2025-04-07）
- **用法**：项目根 `vcpkg.json` 声明依赖（manifest 模式）；CMake 通过
  `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` 启用
- **本项目**：`F:\MolecuLens\vcpkg.json`（含 `builtin-baseline` 锁版本）
  ```powershell
  cd F:\MolecuLens
  F:\vcpkg\vcpkg.exe install --triplet x64-windows
  ```
  triplet 说明：`x64-windows`（动态库，开发默认）；发布可换 `x64-windows-static`

### 1.3 Vulkan SDK（1.3+，本机 1.4.357.0）

- **下载**：<https://vulkan.lunarg.com/sdk/home>（选 Windows 安装器）
- **环境**：安装后设置 `VULKAN_SDK`（本机 `F:\VulkanSDK\1.4.357.0`）；
  `Bin\glslangValidator.exe` 加入 PATH（着色器编译用）
- **组件**：SDK 含 Vulkan 驱动 Loader（vulkan-1.dll）、Validation Layers、
  glslangValidator、shaderc
- **校验层**：debug 构建自动加载 `VK_LAYER_KHRONOS_validation`
  （SDK 安装时注册系统层路径；`VK_LAYER_PATH` 可覆盖）

---

## 2. 必需库（vcpkg manifest 自动安装）

> 全部经 `vcpkg.json` 声明，`vcpkg install` 自动下载编译；版本由
> `builtin-baseline: 0bf1354d6704ec797acea686d0250afc4036a082` 锁定。

### 2.1 GLFW（glfw3，v3.4.x）

- **用途**：窗口创建（`GLFW_NO_API` + Vulkan 表面）、输入轮询、时间
- **基本用法**：
  ```cpp
  glfwInit();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);        // 自绘标题栏（§7.3）
  GLFWwindow* win = glfwCreateWindow(1280, 800, "MolecuLens", nullptr, nullptr);
  glfwGetRequiredInstanceExtensions(&count);          // Vulkan 实例扩展
  glfwCreateWindowSurface(instance, win, nullptr, &surface);
  ```
- **CMake**：`find_package(glfw3 CONFIG REQUIRED)` → 链接 `glfw`
  （Windows 需额外 `winmm`）

### 2.2 Volk（volk，vulkan 1.3+ 版本）

- **用途**：Vulkan 动态加载器（运行时从 vulkan-1.dll 解析函数指针；
  免静态链接 loader，跨平台）
- **基本用法**：
  ```cpp
  volkInitialize();                      // 加载 vulkan-1.dll
  vkCreateInstance(...); volkLoadInstance(instance);   // 实例级加载
  vkCreateDevice(...);  volkLoadDevice(device);        // 设备级加载
  ```
- **CMake**：`find_package(volk CONFIG REQUIRED)` → 链接 `Volk::volk`
  （volk 依赖 vulkan-headers）

### 2.3 Vulkan-Headers（vulkan-headers）

- **用途**：`<vulkan/vulkan.h>` 头文件（项目直接使用官方头）
- **CMake**：`find_package(VulkanHeaders CONFIG REQUIRED)` → `Vulkan::Headers`
- **注意**：若用官方 SDK 头文件可移除本项（vcpkg 版与 SDK 版 API 等价）

### 2.4 VMA（vulkan-memory-allocator，v3.x）

- **用途**：GPU 内存分配器（`vmaCreateBuffer`/`vmaCreateImage` 等；
  缓冲/图像/暂存/宿主可见映射统一管理，主方案 §6.3）
- **基本用法**：
  ```cpp
  #include <vk_mem_alloc.h>
  VmaAllocator allocator;
  vmaCreateAllocator(&(VmaAllocatorCreateInfo{ .device = dev, .physicalDevice = phy,
      .instance = inst }), &allocator);
  vmaCreateBuffer(allocator, &bufInfo, &allocInfo, &buffer, &alloc, nullptr);
  ```
- **CMake**：`find_package(VulkanMemoryAllocator CONFIG REQUIRED)` → `VMA::VMA`

### 2.5 glm（glm，1.0.0）

- **用途**：数学库（`glm::vec3/vec4/mat4/quat`；相机/变换/几何）
- **基本用法**：
  ```cpp
  #include <glm/glm.hpp>
  #include <glm/gtc/matrix_transform.hpp>
  #include <glm/gtc/quaternion.hpp>
  glm::mat4 vp = glm::perspective(glm::radians(fov), aspect, near, far)
                 * glm::lookAt(eye, target, up);
  ```
- **CMake**：`find_package(glm CONFIG REQUIRED)` → `glm::glm`（header-only）

### 2.6 Dear ImGui（imgui，v1.9x + docking-experimental）

- **用途**：全部 UI（自绘标题栏/Dock/面板/时间轴/日志，主方案 §7）
- **安装**：vcpkg feature：
  ```json
  { "name": "imgui", "features": ["docking-experimental"] }
  ```
- **后端**：vcpkg 安装 `include/imgui/backends/imgui_impl_glfw.*` 与
  `imgui_impl_vulkan.*` 源码，由 `src/app/CMakeLists.txt` 直接编译进 app 目标
- **基本用法**：
  ```cpp
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  ImGui_ImplGlfw_InitForVulkan(window, true);
  ImGui_ImplVulkan_Init(&info);          // info: Instance/Device/Queue/RenderPass/...
  // 每帧：ImGui_ImplVulkan_NewFrame(); ImGui_ImplGlfw_NewFrame();
  //       ImGui::NewFrame(); ...UI...; ImGui::Render();
  //       ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  ```
- **CMake**：`find_package(imgui CONFIG REQUIRED)` → `imgui::imgui`

### 2.7 spdlog（spdlog，v1.14+）

- **用途**：分级日志（`[core]/[io]/[engine]/[backend]/[ui]/[python]/[rpc]`；
  异步 + 滚动文件，主方案 §14.5）
- **基本用法**：
  ```cpp
  #include <spdlog/spdlog.h>
  spdlog::set_level(spdlog::level::info);
  spdlog::info("[io] parsed {} with {} reader", path, reader);
  spdlog::error("[backend] Vulkan validation: {}", msg);
  ```
- **CMake**：`find_package(spdlog CONFIG REQUIRED)` → `spdlog::spdlog`

### 2.8 nlohmann-json（nlohmann-json，v3.11+）

- **用途**：`settings.json` / `.mlscene` 元数据段 / `.mlrepr` 表示 JSON
- **基本用法**：
  ```cpp
  #include <nlohmann/json.hpp>
  nlohmann::json j = {{"units", "angstrom"}, {"instances", {...}}};
  std::string s = j.dump(2);
  auto j2 = nlohmann::json::parse(s);
  ```
- **CMake**：`find_package(nlohmann_json CONFIG REQUIRED)` → `nlohmann_json::nlohmann_json`

### 2.9 CLI11（cli11，v2.4+）

- **用途**：CLI 子命令解析（`moleculens-cli render/convert/info`，主方案 §11.3）
- **基本用法**：
  ```cpp
  CLI::App app{"MolecuLens CLI"};
  auto* sub = app.add_subcommand("render", "...");
  sub->add_option("input", input)->required();
  CLI11_PARSE(app, argc, argv);          // 退出码 0/3（用法错误）
  ```
- **CMake**：`find_package(CLI11 CONFIG REQUIRED)` → `CLI11::CLI11`（header-only）

### 2.10 Catch2（catch2，v3.x）

- **用途**：单元/集成测试（`moleculens_tests`）
- **基本用法**：
  ```cpp
  #include <catch2/catch_test_macros.hpp>
  TEST_CASE("name", "[tag]") { REQUIRE(x == y); }
  ```
- **CMake**：
  ```cmake
  find_package(Catch2 CONFIG REQUIRED)
  target_link_libraries(moleculens_tests PRIVATE Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(moleculens_tests)
  ```
- **运行**：`ctest --test-dir build` 或直接运行 `moleculens_tests`

---

## 3. 可选库

### 3.1 pybind11（MOLECULENS_BUILD_PYTHON=ON 时）

- **安装**：`python -m pip install pybind11`（推荐）；或 vcpkg 安装后自行加入
  `vcpkg.json`。默认 manifest **不含** pybind11，避免默认构建拉取 Python 依赖。
- **查找顺序**：`find_package(pybind11 CONFIG)` → 未找到时执行
  `Python3_EXECUTABLE -m pybind11 --cmakedir` 回退到 pip 安装的配置。
- **用法**：绑定函数/类导出 Python 模块，源码在 `python/module/pybind_module.cpp`，
  产物为 `build/python/moleculens/_native.*.pyd`（详见 `python/README.md`）。

### 3.2 cppzmq / libzmq（MOLECULENS_ENABLE_ZMQ=ON 时，非 Windows）

- **vcpkg**：`{ "name": "cppzmq", "platform": "!windows" }`
  （Windows 上 libzmq 编译繁琐，默认不启用）
- **用途**：ZeroMQ REP 套接字 RPC（`tcp://127.0.0.1:5555` + token，主方案 §11.2）

### 3.3 ffmpeg（动画导出 MP4/WebM）

- **下载**：<https://www.gyan.dev/ffmpeg/builds/>（Windows release-full）或
  `winget install Gyan.FFmpeg`
- **用途**：`moleculens-cli animate --codec mp4` 经子进程管道编码
  （`ffmpeg -f rawvideo -pix_fmt rgb24 -i - -c:v libx264 out.mp4`，主方案 §12.6）
- **降级**：无 ffmpeg 时仅支持 PNG 序列 / APNG / GIF（内嵌编码）

---

## 4. 环境配置汇总（Windows 本机）

| 项 | 值 | 用途 |
|---|---|---|
| `VULKAN_SDK` | `F:\VulkanSDK\1.4.357.0` | Vulkan SDK（校验层/glslang） |
| `VCPKG_ROOT` | `F:\vcpkg` | vcpkg manifest 工具链 |
| PATH 追加 | `F:\VulkanSDK\1.4.357.0\Bin` | glslangValidator |
| 编译器 | MSVC 2022（`D:\Microsoft Visual Studio\2022\Community`） | cl.exe 19.44 |
| CMake | `E:\CMake\bin\cmake.exe`（3.31.7） | 构建 |

**一键配置（PowerShell）**：
```powershell
$env:VCPKG_ROOT = "F:\vcpkg"
$env:VULKAN_SDK = "F:\VulkanSDK\1.4.357.0"
$env:Path += ";$env:VULKAN_SDK\Bin"
```

---

## 5. 构建命令（主方案 §14.1 / M0 蓝图 §6）

```powershell
# Windows（MSVC，PowerShell）
cd F:\MolecuLens
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug `
      -DCMAKE_TOOLCHAIN_FILE=F:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Debug -j
.\build\Debug\moleculens_app.exe          # GUI
.\build\Debug\moleculens-cli.exe --version
ctest --test-dir build                     # 单元测试

# Linux / macOS
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DVCPKG_ROOT=$HOME/vcpkg
cmake --build build -j$(nproc)
./build/moleculens_app
```

**可选开关**（CMake）：`MOLECULENS_BUILD_APP/CLI/PYTHON/TESTS`、
`MOLECULENS_ENABLE_ZMQ`、`MOLECULENS_MSAA`（主方案 §14.1）。

---

## 6. 版本锁定与更新

- vcpkg manifest 的 `builtin-baseline` 锁定全部依赖版本（当前
  `0bf1354d6704ec797acea686d0250afc4036a082`）；
- 更新基线：`F:\vcpkg\vcpkg.exe x-update-baseline`（重跑后核对编译）；
- 校验：`F:\vcpkg\vcpkg.exe list` 查看已安装包。
