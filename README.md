# PPet（C++ 重写版）

> 基于 [zenghongtu/PPet](https://github.com/zenghongtu/PPet) 思路，用 **C++23 + OpenGL/GLFW + Qt 6** 从零重写的桌面宠物。
> 保留「透明置顶窗口 + 系统托盘 + 鼠标跟随」的桌面挂件体验，并带来三大增强：
> **全屏追踪**、**VRM 模型支持**、**PMX 模型支持**。

原生桌面程序（无 Electron、无浏览器运行时），启动快、内存占用低，并能在全屏游戏 / 应用中持续跟随光标。

---

## 特性

| 特性 | 说明 |
| --- | --- |
| **全屏追踪** | 通过系统级 API 获取整个虚拟桌面的光标位置，眼睛/头部不再局限于自身窗口（解决原版 issue #26 / #122 / #158） |
| **多模型格式** | VRM 1.0 / 0.x、PMX 2.0 / 2.1、Live2D（可选后端），以及 `.zip` 模型包 |
| **PMX 整包导入** | PMX 不含图形数据，推荐把 `.pmx` + 贴图打包成 `.zip` 直接导入，见 [模型格式](docs/models.md) |
| **透明置顶窗口** | 真·透明叠加层，支持点击穿透、窗口拖动、缩放 |
| **系统托盘** | 置顶 / 忽略点击 / 切换模型 / 语言 / 开机启动 / 调试 / 模型诊断 / 关于 / 退出 |
| **Qt 6 设置面板** | 模型管理、渲染缩放、全屏追踪参数、语言切换 |
| **模型诊断面板** | 实时显示模型的部分、参数、性质与「是否正常渲染」，见 [调试与日志](docs/debugging.md) |
| **结构化日志** | 毫秒级时间戳、源码位置、线程 ID、6 级分级，写到 `./last.log` |
| **中英双语** | 界面文案由 [`Compiler/`](Compiler/README.md) 从 JSON 生成，改文案不必动 C++ |
| **健壮的解析器** | 所有模型读取都有边界检查与索引越界防护，配套 ASAN 模糊测试 |

---

## 快速开始

**要求**：C++23 编译器（GCC 11+ / Clang 14+ / MSVC 19.32+ / llvm-mingw clang 17+）、CMake ≥ 3.20、OpenGL 3.3、GLFW、GLEW、Qt 6 Widgets。

```bash
git clone https://github.com/XaryLight/PPet-RE.git
cd PPet-RE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/ppet
```

构建后跑测试：`cd build && ctest --output-on-failure`。

各平台依赖安装、Live2D 可选后端、Windows 工具链选择与故障排查，见 **[构建指南](docs/build.md)**。

---

## 模型格式支持

| 格式 | 扩展名 | 状态 | 说明 |
| --- | --- | --- | --- |
| VRM 1.0 / 0.x | `.vrm` | 默认支持 | GLB/glTF2.0，含 MToon、humanoid、expressions、springBone |
| PMX 2.0 / 2.1 | `.pmx` | 默认支持 | 二进制，含骨骼蒙皮、材质、变形、显示帧 |
| Live2D | `.model3.json` | 可选 | 需自备 Cubism SDK for Native Core，放入 `src/csdk/` 自动启用 |
| 模型压缩包 | **`.zip`** | 默认支持 | 自动解压并定位包内模型，保留贴图目录结构 |

> **PMX 请优先用 `.zip` 导入**：`.pmx` 文件只记录贴图的文件名，本身不含任何图形数据。
> 单独拖入 `.pmx` 往往得到「能加载但全身纯色」的白模。详见 [模型格式与导入](docs/models.md)。

三种格式在内部统一转换为「蒙皮网格 + toon 着色」表示，共用同一套渲染管线；Live2D 走独立的二维管线。

---

## 使用速览

```bash
./build/bin/ppet
```

首次启动会自动弹出设置面板。右键系统托盘图标可打开菜单，其中：

- **导入模型 / 配置**：选择 `.zip`（推荐）、`.pmx`、`.vrm`、`.json`
- **调试**（`Ctrl+D`）：在日志里打印桌面尺寸、追踪状态与模型摘要
- **模型诊断**（`Ctrl+Shift+D`）：打开 Qt 诊断面板，查看模型部分 / 参数 / 性质 / 渲染状态

> 桌宠是透明无边框窗口，**未加载模型时它本身完全不可见**——这是预期行为，不是 bug。

完整说明见 **[使用与配置](docs/usage.md)** 与 **[调试与日志](docs/debugging.md)**。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/build.md](docs/build.md) | 依赖、C++23 要求、编译步骤、Live2D 后端、Windows 工具链与故障排查 |
| [docs/models.md](docs/models.md) | 模型格式、**PMX 的 `.zip` 包规范**、压缩包导入流程、中文路径 |
| [docs/usage.md](docs/usage.md) | 运行、托盘菜单、设置面板、全屏追踪、配置字段 |
| [docs/debugging.md](docs/debugging.md) | 模型诊断面板、结构化日志格式与级别、常见问题排查 |
| [docs/development.md](docs/development.md) | 项目结构、架构分层、测试清单、文案翻译器、C++23 使用点 |
| [Compiler/README.md](Compiler/README.md) | 文案翻译器（JSON/CSV → C++ 内嵌表）的用法 |

---

## 项目结构

```
PPet-RE/
├── CMakeLists.txt            # 构建配置（模型层 / 平台层 / 主程序 / 测试）
├── src/
│   ├── core/                 # 配置、国际化、结构化日志、文件读写、ZIP 解压、模型包
│   ├── model/                # 模型抽象 + PMX / VRM / Live2D 后端 + 统一蒙皮网格
│   ├── render/               # toon 着色渲染管线、相机、工具
│   ├── platform/             # 窗口、托盘、全局光标追踪、开机启动、文件对话框
│   ├── ui/                   # Qt 6 设置面板与模型诊断面板
│   ├── app/                  # 主循环与应用编排
│   └── main.cpp              # 程序入口
├── src/csdk/                 # Cubism SDK Core（可选，需自备，已 gitignore）
├── Compiler/                 # 文案翻译器：JSON/CSV → C++ 内嵌表
├── docs/                     # 分类文档
├── third_party/              # nlohmann/json、stb、zlib（源码随仓库）
└── tools/                    # 测试素材生成脚本与测试程序
```

详见 [开发文档](docs/development.md)。

---

## 测试

```bash
cd build
ctest --output-on-failure
```

覆盖 PMX/VRM 解析、ZIP 模型包（含 PMX 贴图容错）、中文路径、渲染冒烟、Qt 界面、结构化日志、文案翻译器等。完整清单见 [开发文档 · 测试](docs/development.md#测试)。

---

## 致谢

- 原始项目：[zenghongtu/PPet](https://github.com/zenghongtu/PPet)（Electron + Live2D 桌面宠物）
- [Live2D Cubism SDK](https://www.live2d.com/)（专有许可，可选后端）
- [Qt 6](https://www.qt.io/)、[GLFW](https://www.glfw.org/)、[GLEW](https://glew.sourceforge.net/)、[nlohmann/json](https://github.com/nlohmann/json)、[stb](https://github.com/nothings/stb)

## 许可证

Apache License 2.0，见 [LICENSE](./LICENSE)。

> Live2D Cubism SDK 采用其自有许可，不在本仓库许可范围内；使用 Live2D 后端时请遵守 Cubism SDK 的许可条款。
