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
| **系统托盘** | 置顶 / 忽略点击 / 切换模型 / **表情预设** / **说一句** / 台词气泡开关 / 语言 / 开机启动 / 调试 / 模型诊断 / 关于 / 退出 |
| **Qt 6 设置面板** | 模型管理、渲染缩放、全屏追踪参数、语言切换 |
| **模型诊断面板** | 7 个页签：概览与渲染、**模型 ID**、参数、**骨架信息**、**图片渲染（贴图缩略图 + 实时渲染帧）**、模型部分、性质与详细错误、**表情与说话**，见 [调试与日志](docs/debugging.md) |
| **表情管理** | PMX / VRM 表情预设 + 别名解析 + 平滑过渡 + 自动眨眼叠层；Live2D 表情动作组，见 [表情管理与语言功能](docs/expressions.md) |
| **语言功能** | 台词库（按状态分档）/ 台词气泡 / 音节包络口型同步，三种模型格式共用，见 [表情管理与语言功能](docs/expressions.md) |
| **失败原因弹窗** | 添加模型失败时用 Qt 弹窗显示分类（未找到有效骨架 / 模型图片缺失 / 解析错误 / moc3 错误…）、逐条明细与建议 |
| **结构化日志** | 毫秒级时间戳、源码位置、线程 ID、6 级分级，写到 `./last.log` |
| **中英双语** | 界面文案由 [`Compiler/`](Compiler/README.md) 从 JSON 生成，改文案不必动 C++ |
| **渲染验证工具** | `ppet_render_verify` 对 VRM / PMX(zip) / moc3 出图并输出 PNG + JSON 报告，见 [Windows 验收](docs/windows-test.md) |
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
| Live2D | `.model3.json` | 可选 | 需自备 Cubism SDK for Native Core，放入 `api/live2d/csdk/` 自动启用 |
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
- **表情**：切换表情预设（自然 / 开心 / 放松 / 生气 / 难过 / 惊讶），带平滑过渡
- **说一句**：让桌宠随机说一句；**台词气泡**开关控制是否显示文字气泡
- **调试**（`Ctrl+D`）：在日志里打印桌面尺寸、追踪状态与模型摘要
- **模型诊断**（`Ctrl+Shift+D`）：打开 Qt 诊断面板，查看模型部分 / 参数 / 性质 / 渲染状态 / 表情与说话

> 桌宠是透明无边框窗口，**未加载模型时它本身完全不可见**——这是预期行为，不是 bug。

完整说明见 **[使用与配置](docs/usage.md)**、**[表情管理与语言功能](docs/expressions.md)**、
**[Live2D 分层运行时](docs/live2d-stage.md)** 与 **[调试与日志](docs/debugging.md)**。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | **项目结构、根索引约定、`src/ui` / `src/tools` / `api` 分工**、旧新路径速查 |
| [docs/build.md](docs/build.md) | 依赖、C++23 要求、编译步骤、Live2D 后端、Windows 工具链与故障排查 |
| [docs/windows-test.md](docs/windows-test.md) | **Windows 11 + N 卡 + llvm-mingw + Qt6 + SDK 5.x 真机验收手册** |
| [docs/models.md](docs/models.md) | 模型格式、**PMX 的 `.zip` 包规范**、压缩包导入流程、中文路径 |
| [docs/expressions.md](docs/expressions.md) | **表情管理（预设 / 别名 / 过渡 / 叠加层）、语言功能（台词 / 气泡 / 口型）** |
| [docs/live2d-stage.md](docs/live2d-stage.md) | **Live2D 分层运行时：参数栈 / 姿态总线 / 三通道视线 / 待机 / 热区 / 口型** |
| [docs/usage.md](docs/usage.md) | 运行、托盘菜单、设置面板、全屏追踪、配置字段 |
| [docs/debugging.md](docs/debugging.md) | 模型诊断面板、失败弹窗、结构化日志格式与级别、常见问题排查 |
| [docs/development.md](docs/development.md) | 架构分层、测试清单、文案翻译器、C++23 使用点 |
| [api/README.md](api/README.md) | Live2D SDK 集成层与第三方库引用头说明 |
| [Compiler/README.md](Compiler/README.md) | 文案翻译器（JSON/CSV → C++ 内嵌表）的用法 |

---

## 项目结构

```
PPet-RE/
├── CMakeLists.txt            # 构建配置（C++23 / Qt6 / Cubism / 测试）
├── src/                      # 产品源码：只有 ui 与 tools 两个文件夹
│   ├── ui/                   # ★ 所有 GUI 实现：SettingsPanel / DebugPanel / BubbleWidget / ErrorDialog
│   └── tools/                # ★ 所有非 GUI 实现
│       ├── main.cpp          # 程序入口
│       ├── app/              # 主循环、托盘菜单、应用编排
│       ├── agent/            # 语言系统：台词库 / 气泡状态机 / 口型
│       ├── core/             # 配置、i18n、结构化日志、文件 IO、ZIP、模型包
│       ├── model/            # IModel 抽象、蒙皮网格、表情调度器、PMX / VRM 后端、错误模型
│       ├── platform/         # 窗口、托盘、光标追踪、开机启动、文件对话框
│       └── render/           # 渲染管线、相机、渲染帧抓图
├── api/                      # ★ 外部 SDK / 第三方库引用层
│   ├── live2d/               # Cubism SDK 集成（model3.json / moc3 / 动作 / 分层运行时）
│   │   └── csdk/             # 自备的 Cubism Core（已 gitignore）
│   └── vendor/               # nlohmann/json、stb、zlib 统一引用头
├── Compiler/                 # 文案翻译器：JSON/CSV → C++ 内嵌表
├── docs/                     # 分类文档（含架构说明与 Windows 验收手册）
├── third_party/              # 第三方源码（nlohmann / stb / zlib）
└── tools/                    # 测试程序、素材生成、渲染验证 CLI、Windows 验收脚本
```

头文件统一使用**从根目录起算的尖括号索引**（`#include <src/ui/DebugPanel.hpp>`），
详见 **[项目结构与头文件索引](docs/architecture.md)**。

---

## 测试

```bash
cd build
ctest --output-on-failure
```

覆盖 PMX/VRM 解析、ZIP 模型包（含 PMX 贴图容错）、中文路径、渲染冒烟、Qt 界面、结构化日志、文案翻译器、表情调度、语言系统、Live2D 分层运行时等，共 **28 项**。完整清单见 [开发文档 · 测试](docs/development.md#测试)。

---

## 致谢

- 原始项目：[zenghongtu/PPet](https://github.com/zenghongtu/PPet)（Electron + Live2D 桌面宠物）
- 表情管理参考：[Tanorika/VRM-VPet](https://github.com/Tanorika/VRM-VPet)（表情预设 / 过渡 / 时间轴）
- 语言功能参考：[FULASKA/-live2d-](https://github.com/FULASKA/-live2d-)（台词 / 气泡 / 口型 / 分层运行时）
- [Live2D Cubism SDK](https://www.live2d.com/)（专有许可，可选后端）
- [Qt 6](https://www.qt.io/)、[GLFW](https://www.glfw.org/)、[GLEW](https://glew.sourceforge.net/)、[nlohmann/json](https://github.com/nlohmann/json)、[stb](https://github.com/nothings/stb)

## 许可证

Apache License 2.0，见 [LICENSE](./LICENSE)。

> Live2D Cubism SDK 采用其自有许可，不在本仓库许可范围内；使用 Live2D 后端时请遵守 Cubism SDK 的许可条款。
