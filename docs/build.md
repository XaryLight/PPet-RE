# 构建指南

← [返回 README](../README.md) · [文档索引](README.md)

---

## 环境要求

| 项目 | 最低版本 | 说明 |
| --- | --- | --- |
| **C++ 标准** | **C++23** | 代码使用了 `std::source_location`、`std::string::contains`、`std::to_underlying`、`std::string_view::starts_with` 等现代设施 |
| 编译器 | GCC 11+ / Clang 14+ / MSVC 19.32+（VS 2022 17.2+）/ llvm-mingw clang 17+ | CMake 在配置阶段检查 `cxx_std_23`，不支持会直接给出中文报错 |
| CMake | **3.20+** | CMake 从 3.20 才认识 `CXX_STANDARD=23` / `cxx_std_23`（原来要求 3.16） |
| 图形 | 桌面 OpenGL 3.3 核心、[GLFW](https://www.glfw.org/)、[GLEW](https://glew.sourceforge.net/) | GLFW 缺失时 CMake 会用 FetchContent 自动从源码构建 |
| 界面 | [Qt 6](https://www.qt.io/) Widgets | 设置面板与模型诊断面板用 `QDialog` 实现 |

> **为什么不用 C++23 的新库特性**：`std::format` 在 GCC 13 才落地、`std::ranges::to` 要 GCC 14，
> 而项目要同时支持「Linux GCC 11」与「Windows llvm-mingw（可能用 libc++）」。
> 因此跨平台代码只用**广泛实现的语言特性与小型库特性**，不依赖 `<format>` / `<expected>`，
> 并且对 `contains` / `to_underlying` 这类特性做了 `#if __cpp_lib_...` 回退保护。

代码里实际用到的 C++23 / C++20 设施：

| 特性 | 用在哪 |
| --- | --- |
| `std::source_location` | 日志自动带「文件:行号 函数」，不用手写 `__FILE__` |
| `std::string::contains` | 模型类型判断、Live2D manifest 判断 |
| `std::to_underlying` | 日志级别转数组下标 |
| `std::string_view::starts_with` | `main.cpp` 解析 `--log-level=` 命令行参数 |
| `std::size` / 结构化绑定 / 类模板参数推导 | 各模块的常规写法 |

---

## 依赖安装

### Linux（Ubuntu / Debian）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
    libglfw3-dev libglew-dev libgl1-mesa-dev libglu1-mesa-dev \
    libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
    libxext-dev libxft-dev libfontconfig1-dev libssl-dev \
    qt6-base-dev
# 可选：无头环境下跑图形测试
sudo apt-get install -y xvfb
```

### Windows

使用 vcpkg 或 MSYS2 安装 `glfw3`、`glew`，并准备 Qt 6。

```powershell
vcpkg install glfw3 glew
```

CLion + llvm-mingw 用户见下方「[Windows 工具链选择](#windows-工具链选择重要)」——**必须让 SDK 走 DLL 导入库方案**。

### macOS

```bash
brew install cmake glfw glew qt@6
```

---

## 编译步骤

```bash
git clone https://github.com/XaryLight/PPet-RE.git
cd PPet-RE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

产物：

- `build/bin/ppet` —— 主程序
- `build/bin/ppet_*_test` / `build/bin/ppet_*_smoke` —— 测试程序（部分在构建根目录下）

找不到 Qt 时显式指定：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt6 安装路径>
```

### 构建选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `PPET_WITH_CUBISM` | `OFF` | 启用真实 Live2D Cubism SDK 后端 |
| `PPET_WITH_CUBISM_STUB` | `OFF` | 用内置 Cubism Core 桩验证构建链路（不能加载真实模型） |
| `PPET_CUBISM_AUTODETECT` | `ON` | 自动检测 `api/live2d/csdk/` 下的 SDK；**显式传 `PPET_WITH_CUBISM=ON/OFF` 时以显式值为准** |
| `PPET_CUBISM_ROOT` | 空 | 手动指定 Core 目录（可指向 Core 本身或其上层） |
| `PPET_I18N_ZH` / `PPET_I18N_EN` | `Compiler/i18n/*.json` | 文案源文件，可改成 CSV |

---

## 启用 Live2D 后端（可选）

Live2D Cubism SDK 为**专有许可软件，不可随本仓库分发**。若要支持 `.model3.json`，用的是 **Cubism SDK for Native** 的 **Core 层**：

1. 从 <https://www.live2d.com/en/sdk/download/> 下载 **Cubism SDK for Native**（只需其中的 **Core** 目录）；
2. 把 **Core 目录**放到 **`api/live2d/csdk/`**（或 `third_party/Cubism/`，或用 `-DPPET_CUBISM_ROOT=<目录>` 指定），最终结构应含：
   - `api/live2d/csdk/Core/include/Live2DCubismCore.h`
   - `api/live2d/csdk/Core/lib/linux/x86_64/libLive2DCubismCore.a`（Linux）
   - `api/live2d/csdk/Core/lib/macos/libLive2DCubismCore.a`（macOS）
   - Windows（见下）：
     - `api/live2d/csdk/Core/dll/windows/x86_64/Live2DCubismCore.dll` + `Live2DCubismCore.lib`（**推荐，MinGW 必用**）
     - `api/live2d/csdk/Core/lib/windows/x86_64/<版本号>/Live2DCubismCore_MD.lib`（MSVC 可用）
3. 重新配置并构建：

   ```bash
   cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt6 安装路径>
   cmake --build build
   ```

**CMake 会自动检测 `api/live2d/csdk/` 下的 SDK 并启用 Live2D 后端**，无需手工传 `-DPPET_WITH_CUBISM=ON`。
配置阶段会打印 `检测到 Cubism SDK（...），自动启用 Live2D 后端` 以示确认。

> **显式传 `-DPPET_WITH_CUBISM=OFF` 时自动检测会被跳过**，后端保持关闭。
> 自动检测只在「用户没有表态」时生效；想让它一直自动生效但临时关掉后端，
> 可以用 `-DPPET_CUBISM_AUTODETECT=OFF`。
>
> 这是通过在 `option()` 之前用 `DEFINED CACHE{PPET_WITH_CUBISM}` 判断用户是否显式传参实现的 ——
> `option()` 与 `-D` 在 `CMakeCache.txt` 里长得一模一样（都是 `FOO:BOOL=OFF`），
> 旧版本无法区分，只能用 `FORCE` 覆盖，导致用户明明写了 OFF 仍被改回 ON。
>
> `api/live2d/csdk/` 与 `third_party/Cubism/` 已加入 `.gitignore`，专有 SDK 不会被提交。

不下载 SDK 也能验证构建与 UI 链路：`-DPPET_WITH_CUBISM_STUB=ON` 会启用内置的
Cubism Core **桩**（同名同签名的 `csm*` 函数），可编译出带 Live2D 入口的程序，但不能加载真实模型。

> **Cubism Core 4.x 与 5.x/6.x 的 API 不同**：绘制顺序函数在 4.x 是
> `csmGetDrawableRenderOrders`、在 5.x+ 是 `csmGetRenderOrders`。
> CMake 会读取 SDK 头文件自动判断并定义对应宏，两种 SDK 都能编译，无需改源码。
>
> 两个开关都未开启时，加载 Live2D 模型会给出「需要编译 Cubism SDK 后端」的友好提示，不影响其它功能。

注：Live2D 走独立的**二维管线**（按部件网格叠加绘制），与 PMX/VRM 的蒙皮网格 + toon 着色管线不同；两者共用透明置顶窗口与全屏追踪。

---

## Windows 工具链选择（重要）

Cubism SDK for Native 只提供 **MSVC 编译**的产物，不同 Windows 工具链的处理方式差别很大：

| 工具链 | 用哪个库 | 说明 |
| --- | --- | --- |
| **MSVC**（Visual Studio） | `lib/windows/.../Live2DCubismCore_MD.lib` | MSVC 链接器能正确处理 `.drectve` 里的 `/DEFAULTLIB` 与 `OLDNAMES` 指令，直接用静态库即可 |
| **MinGW / llvm-mingw**（CLion 等） | **`dll/windows/x86_64/Live2DCubismCore.dll` + `.lib` 导入库** | **必须用 DLL 方案**，见下 |

### 为什么 MinGW 必须用 DLL

Cubism 的静态库是用 MSVC 编译的，内部引用了一批 MSVC CRT 专有符号
（`__security_cookie`、`__GSHandlerCheck`、`__imp___stdio_common_vfprintf` 等），
并靠 `.drectve` 里的 `/DEFAULTLIB:"MSVCRT"`、`OLDNAMES` 指令告诉链接器如何接 CRT。
GNU ld / lld 无法完整解析这些指令，会出现三类错误：

1. **`could not open 'libLIBCMT.a'`** —— lld 去解析 `.drectve` 里的 `/DEFAULTLIB:"LIBCMT"`，MinGW 工具链没有这个库；
2. **undefined reference** —— 缺少 `__security_cookie` 等符号；
3. **multiple definition** —— SDK 库里同时存在 `PlatformLogger.c` 与 `PlatformLogger_Win.c`
   两套互斥实现（都定义 `__local_stdio_printf_options`），GNU ld 按需拉取静态库成员时会把两个都拉进来。

动态库方案把整个 Cubism Core 连同它的 MSVC 运行时依赖封进 `Live2DCubismCore.dll`，
链接时只需要一个干净的导入库，上述问题全部消失。
CMake 在检测到 MinGW 且存在 DLL 导入库时会**自动选用 DLL 方案**，
并把 `Live2DCubismCore.dll` 自动拷贝到可执行文件旁边。

配置阶段会看到：

```
-- Live2D 后端：MinGW —— 使用 Cubism Core 动态库导入库（.../dll/windows/x86_64/Live2DCubismCore.lib）
-- Live2D 后端：将自动拷贝 .../Live2DCubismCore.dll 到输出目录
```

> **MinGW 下不会再回退到静态库。** 找不到 `dll/windows/x86_64/Live2DCubismCore.lib` 时，
> CMake 在配置阶段直接报错并给出三种解法（用 DLL 方案 / 换 MSVC 工具链 / `-DPPET_WITH_CUBISM=OFF`），
> 而不是等到链接时才抛出 `libLIBCMT.a` 这种天书错误。
>
> 另外 CMake 用「显式路径 + 文件存在性判断」而不是 `find_library()` 定位 SDK 里的库 ——
> 交叉编译时 `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY` 会把搜索路径改写到 sysroot 下，
> 导致「文件明明在源码目录里，find_library 却说找不到」。

### 运行时 DLL 部署

`0xC0000135` = `STATUS_DLL_NOT_FOUND`，即 Windows 加载器找不到依赖 DLL。构建后 CMake 会自动拷贝：

- `Live2DCubismCore.dll`（启用 Live2D 时）
- MinGW 运行时：`libgcc_s_seh-1.dll` / `libstdc++-6.dll` / `libwinpthread-1.dll`

Qt 6 的依赖仍需用 `windeployqt` 部署到 `bin/` 目录。

---

## Windows 常见故障排查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `ld.lld: error: could not open 'libLIBCMT.a'` | 链接到了 MSVC 静态库 `Live2DCubismCore_MT.lib` | 确认 SDK 里有 `dll/windows/x86_64/Live2DCubismCore.lib`，并 `git pull` 最新版 CMakeLists（新版在配置阶段就会拦截） |
| 退出码 `3221225781 (0xC0000135)` | `STATUS_DLL_NOT_FOUND` | 多为 `Live2DCubismCore.dll` 或 MinGW 运行时 DLL 没跟着 exe 走；用 `windeployqt` 部署 Qt 依赖 |
| 改了 `-DPPET_WITH_CUBISM=OFF` 却仍在链接 Cubism | 旧版 CMake 的自动检测会 `FORCE` 覆盖用户设置 | 升级到当前版本；也可加 `-DPPET_CUBISM_AUTODETECT=OFF` 双保险 |
| `CMake Error: 未找到 Live2DCubismCore.h` | SDK 目录结构不对 | 确认 `<SDK>/include/Live2DCubismCore.h` 存在，或用 `-DPPET_CUBISM_ROOT=<Core 目录>` 显式指定 |
| `CXX_STANDARD is set to invalid value '23'` | CMake 版本过老（< 3.20） | 升级 CMake 到 3.20+（CLion 自带版本通常已满足） |
| 当前编译器不支持 C++23 | 编译器版本过低 | 升级到表「环境要求」列出的最低版本 |
| `Cannot find source file: src/core/FileIO.cpp` / `No SOURCES given to target: ppet_model` | **本地 `CMakeLists.txt` 与 `src/` 目录版本不匹配**：CMakeLists 还是 `34da636` 之前的旧版（引用 `src/core/...`），而源码已是新结构 `src/tools/...` | 见下方「版本不同步自查」 |

### 版本不同步自查

`34da636` 起目录调整为 `src/{ui,tools}` + `api/`。如果只更新了部分文件（常见于本地手改过 `CMakeLists.txt` 导致 `git pull` 被跳过），configure 会抛出一串 `Cannot find source file` / `No SOURCES given to target`，报错本身看不出是版本问题。

先跑诊断脚本（只报告、不改文件）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_sync.ps1
```

最小修复（只同步构建脚本 + 清缓存，保留其他本地改动）：

```powershell
git fetch origin
git checkout origin/main -- CMakeLists.txt
Remove-Item -Recurse -Force cmake-build-debug
```

彻底修复（先 `stash` 备份所有本地改动，再整体对齐远程）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_sync.ps1 -Fix
```

完成后在 CLion 里 **Reload CMake Project**。新版 `CMakeLists.txt` 自带源码树一致性自检，若 `src/` 结构与脚本不匹配会在配置阶段直接给出中文提示。

> 提示：新版已支持 `-DPPET_WITH_CUBISM=OFF` 真正生效（不再被自动检测 `FORCE` 覆盖），**不需要再手改 `CMakeLists.txt`**——这正是过去反复产生本地改动、导致 `git pull` 不生效的根源。

### 运行期：诊断面板全绿，屏幕却看不到模型

**现象**：诊断面板显示「正常渲染」、GPU 就绪、GL 错误 0、Draw calls 62、贴图 11/11/0、FPS 58、帧数持续增长，但桌面上就是没有模型（`Win+Tab` 里也未必看得到）。

**根因**：主循环有模型时只调 `Update/Render`，**漏了每帧 `glClear`**。双缓冲交换后 back buffer 内容按 GL 规范未定义，深度缓冲又残留上一帧的值，第二帧起所有片元因深度相等被 `GL_LESS` 剔除——draw calls 照发、GL 不报错、FPS 正常，帧缓冲却一个非透明像素都没有。另一半原因是旧版 `renderingOk` 只看「网格非空 + GPU 就绪 + GL 错误为 0」，这些全是间接指标，证明不了有像素。

**已修复**：主循环每帧渲染前无条件调用 `ClearTransparent()`，且该函数显式复位 `glColorMask` / `glDepthMask`（半透明材质会把 `glDepthMask` 留成 `GL_FALSE`，不清掉的话 `glClear(GL_DEPTH_BUFFER_BIT)` 等于没执行）。同时诊断结论改为以**帧缓冲读回的非透明像素覆盖率**为最终裁决，面板「渲染状态」第一行显示 `实际出图 Pixels`，为 `0.00% ← 未出图` 时状态灯转红。

**判定与复现**（`git pull` 后应自动恢复）：

```bash
# 正确主循环：每帧清屏 + 交换，覆盖率应稳定 > 0
ppet_render_verify --model model.pmx --frames 8 --swap-each-frame --report clear.json

# 复现旧 bug：漏清屏，覆盖率应为 0%，但 rendering_ok 仍是 true
ppet_render_verify --model model.pmx --frames 8 --no-clear --swap-each-frame --report noclear.json
```

对照两份报告的 `render.coverage_percent` 与 `render.rendering_ok`。CTest 的 `render_verify_clear_contract` 已把这条契约固化为回归测试。详见 [调试与日志](debugging.md#模型诊断面板)。

---

## 渲染验证与真机验收

构建会额外产出 `ppet_render_verify`（统一渲染验证 CLI）：

```bash
./build/ppet_render_verify --model <.vrm|.pmx|.zip|.model3.json|.moc3> \
    --shot shot.png --report report.json --frames 30
```

它创建隐藏 GL 上下文，加载模型、渲染若干帧、用 `glReadPixels` 统计非透明覆盖率与 GL 错误，
输出 PNG 截图和 JSON 报告（`result` = `PASS` / `PASS_WITH_WARNINGS` / `FAIL`）。
`ctest` 里的 `render_verify_pmx` / `render_verify_vrm` 会对仓库自带素材自动跑一次（需 xvfb）。

**Windows 11 + N 卡 + llvm-mingw + Qt6 + Live2D SDK 5.x 的完整验收**（全编译 → 试运行 →
三类模型加载 → 渲染截图）见 **[Windows 真机验收](windows-test.md)** 与
[`tools/windows_verify.ps1`](../tools/windows_verify.ps1)。

项目目录与头文件索引约定见 [架构文档](architecture.md)。
