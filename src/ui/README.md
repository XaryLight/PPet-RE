# `src/ui/` — 全部 GUI 实现

按 [架构文档](../../docs/architecture.md) 的约定，`src/` 下只有两个文件夹：
`ui/`（本目录，全部 Qt 6 Widgets 界面）与 [`tools/`](../tools/README.md)（全部非 GUI 实现）。

| 文件 | 作用 |
| --- | --- |
| `SettingsPanel.*` | 设置窗口：模型列表、导入、渲染缩放、全屏追踪参数、语言、关于 |
| `DebugPanel.*` | 模型诊断面板：7 个页签（概览与渲染/模型 ID/参数/骨架/图片渲染/模型部分/性质与错误、表情与说话） |
| `BubbleWidget.*` | 台词气泡窗口：跟随桌宠、不抢置顶、最大宽度与字号跟随渲染窗口 |
| `ErrorDialog.*` | 模型加载失败或警告弹窗：分类、逐条明细、建议、日志路径、复制详情 |

## 共同约定

- **不是 QObject 子类**，不需要 `moc` / `AUTOMOC`；按钮回调用 lambda + `QObject::connect`。
- `QApplication` 在 `src/tools/main.cpp` 创建一次；`App` 主循环每帧调用各面板的 `PumpEvents()`
  驱动 Qt 事件分发，与 GLFW 共用同一条主线程，不额外开 Qt 线程。
- 窗口一律**非模态**（`show()` 而非 `exec()`），避免阻塞渲染主循环。
- 面板不可见时 `Update()` 立即返回（零开销），可见时做 200 ms 节流。
- 文案来自 `Strings`（由 [`Compiler/`](../../Compiler/README.md) 从 JSON 生成），切换语言时 `Refresh()` 重绘。

## 头文件根索引

```cpp
#include <src/ui/DebugPanel.hpp>
#include <src/tools/core/I18n.hpp>
```
