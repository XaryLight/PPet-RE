# `src/tools/` — 非 GUI 产品实现

按 [架构文档](../../docs/architecture.md) 的约定，`src/` 下只有两个文件夹：
`ui/`（全部 GUI）与 `tools/`（全部非 GUI）。本目录是后者，**会被编进 `ppet.exe`**。

> 根目录还有一个 [`tools/`](../../tools) —— 那是测试程序、素材生成与验收脚本，不进产品。
> 两者不要混淆。

| 子目录 | 职责 |
| --- | --- |
| `main.cpp` | 程序入口：`QApplication` → `Logger::Init` → `App` |
| `app/` | 主循环、托盘菜单编排、模型加载、运行时状态采集、失败弹窗接线、交互式控制台执行层 |
| `agent/` | 语言系统：台词库 / 气泡状态机 / 口型（纯逻辑，零依赖） |
| `core/` | 配置持久化、i18n 文案结构、结构化日志、UTF-8 文件 IO、ZIP 解析、模型包导入、模型命名、长期稳定性、追踪策略 |
| `model/` | `IModel` 抽象与诊断快照、`ModelLoadError` 错误分类、`SkinnedMesh`、程序化待机 / 骨骼动作、动作识别、PMX / VRM 后端 |
| `platform/` | 窗口（GLFW）、系统托盘、全局光标追踪、开机启动、文件对话框 |
| `plugin/` | 插件系统：`IPlugin` / `PluginManager` / 事件总线 / 内置示例插件 |
| `render/` | toon 着色渲染管线、相机、GL 错误检查、渲染帧抓图 |

**约束**：本目录的代码不依赖 Qt（Qt 只出现在 `src/ui/` 与入口的 `QApplication`），
这样绝大多数逻辑可以在无图形环境下单元测试。

头文件统一用根索引：

```cpp
#include <src/tools/core/Log.hpp>
#include <src/tools/model/ModelError.hpp>
```

外部 SDK / 第三方库的适配在 [`api/`](../../api/README.md)，不在本目录。
