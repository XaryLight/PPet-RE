# `api/` — 外部依赖集成层

本目录存放**所有第三方 SDK / 库的引用与适配代码**，与产品实现（[`src/tools/`](../src/tools/README.md)）和界面实现（`src/ui/`）隔离。

## 为什么要单独一层

桌宠本体不关心「Cubism Core 的符号从哪个 `.lib` 里来」「JSON 用的是哪个库」。把这些外部依赖集中到 `api/`，换来三件事：

1. **换库/升级不扩散**：例如把 nlohmann/json 换成别的，只改 `api/vendor/Json.hpp`；
2. **SDK 放置位置统一**：Cubism SDK for Native 的 Core 目录放 `api/live2d/csdk/Core/`，CMake 自动识别；
3. **产品代码可测试**：`src/tools/` 里的解析逻辑不直接 `#include` 专有 SDK 头，测试无需 SDK 也能编译。

## 目录

| 路径 | 职责 |
| --- | --- |
| `live2d/` | **Live2D Cubism SDK 集成层**：`.model3.json` 清单解析、`.moc3` 加载（`CubismCoreAdapter`）、动作/表情、`Live2DModel` 后端（实现 `IModel`） |
| `live2d/csdk/` | 用户自备的 Cubism SDK for Native **Core** 目录（专有许可，已 gitignore，不随仓库分发）。CMake 会自动探测；旧位置 `src/csdk/`、`third_party/Cubism/Core/` 仍兼容 |
| `vendor/Json.hpp` | nlohmann/json 统一引用点 |
| `vendor/ImageIo.hpp` | stb_image 统一引用点（贴图解码） |
| `vendor/Compression.hpp` | zlib 统一引用点（ZIP 模型包解压） |

`vendor/` 只提供**引用头**（一行 `#include` + 别名），不含第三方源码；源码在 [`third_party/`](../third_party)。
注意 stb 是 header-only 且需要在一个翻译单元里定义 `STB_IMAGE_IMPLEMENTATION` / `STB_IMAGE_WRITE_IMPLEMENTATION`，因此这两个实现宏仍由具体使用方定义，`ImageIo.hpp` 只负责给出声明。

## 头文件索引约定

全项目统一使用**从 workspace 根目录起算**的尖括号索引：

```cpp
#include <api/vendor/Json.hpp>
#include <api/live2d/Live2DManifest.hpp>
#include <src/tools/core/Log.hpp>
#include <src/ui/DebugPanel.hpp>
```

CMake 因此把 `CMAKE_CURRENT_SOURCE_DIR`（项目根）加入 include 路径，而不是 `src/`。
第三方/系统头保留库名前缀（`<nlohmann/json.hpp>`、`<GL/glew.h>`、`<QApplication>`），
不套 `src/` 前缀 —— 它们不是本工程的源码。

详见 [`docs/architecture.md`](../docs/architecture.md)。
