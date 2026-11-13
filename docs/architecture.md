# 项目结构与头文件索引

本页说明 T2 结构优化后的目录约定。**所有路径都从 workspace 根目录起算**。

```
PPet/
├── CMakeLists.txt          # 构建配置（C++23 / Qt6 / Cubism / 测试）
├── README.md               # 概览 + 快速开始 + 文档索引
├── src/                    # 产品源码（只有 ui / tools 两个文件夹）
│   ├── ui/                 # ★ 所有 GUI 实现（Qt 6 Widgets）
│   │   ├── SettingsPanel.*     # 设置窗口
│   │   ├── DebugPanel.*        # 模型诊断面板（7 个页签）
│   │   ├── BubbleWidget.*      # 台词气泡窗口（跟随桌宠，不抢置顶）
│   │   └── ErrorDialog.*       # 模型加载失败/警告弹窗
│   └── tools/              # ★ 所有非 GUI 实现（按职责分子目录）
│       ├── main.cpp            # 程序入口（QApplication + App）
│       ├── app/                # 应用主控制器、托盘菜单、主循环
│       ├── agent/              # 语言系统：台词库 / 气泡状态机 / 口型（纯逻辑）
│       ├── core/               # 配置、日志、i18n、文件 IO、ZIP 解析、模型包
│       ├── model/              # IModel 抽象、SkinnedMesh、表情调度器、程序化待机 / 骨骼动作、PMX / VRM 后端、错误模型
│       │   ├── IdleAnimator.hpp    # ★ 程序化待机微动（纯头文件，零依赖）
│       │   └── BoneMotion.*        # ★ 骨骼动作（点头/摇头/挥手 + 随机触发）
│       ├── platform/           # 窗口、托盘、开机启动、光标追踪、文件对话框
│       └── render/             # Renderer / Camera / RenderUtils / FrameCapture
├── api/                    # ★ 外部 SDK / 第三方库的引用与适配层
│   ├── live2d/             # Cubism SDK 集成：model3.json / moc3 / 动作 / 物理 / Live2DModel
│   │   ├── MotionDirector.hpp  # ★ 动作调度（纯逻辑：待机/互动/优先级/fade，可脱离 SDK 单测）
│   │   ├── Stage.hpp          # ★ 分层运行时（纯逻辑：参数栈/视线/待机/热区/口型/小表情）
│   │   ├── Live2DPhysics.*    # ★ physics3.json 解析 + Cubism 官方摆锤求解（可脱离 SDK 单测）
│   │   ├── Live2DModel.*       # 模型生命周期 / 参数驱动 / 绘制
│   │   ├── Live2DManifest.*    # model3.json 解析（含 HitAreas）
│   │   ├── Live2DMotion.*      # motion3.json 曲线解析（含 Fade / Meta.Loop / Segments）
│   │   ├── CubismCore*         # SDK 唯一适配点
│   │   └── csdk/           # 用户自备的 Cubism Core（gitignore，不入库）
│   └── vendor/             # nlohmann/json、stb、zlib 的统一引用头
├── third_party/            # 第三方源码（nlohmann / stb / zlib），随仓库分发
├── tools/                  # 测试程序、素材生成脚本、Windows 验收脚本
│   ├── test_stubs/         # Cubism / Renderer 测试桩
│   ├── testdata/           # 测试用 .pmx / .vrm / .zip 素材
│   ├── *_test.cpp / *_smoke.cpp
│   ├── render_verify.cpp   # 统一渲染验证 CLI（截图 + JSON 报告）
│   └── windows_verify.ps1  # Windows 11 真机一键验收
├── Compiler/               # 文案翻译器（JSON/CSV -> C++ constexpr 表，见 Compiler/README.md）
└── docs/                   # 文档（本页所在）
```

> **两个 `tools` 不要混淆**
> - `src/tools/` —— 运行时**产品实现**（非 GUI 部分），会被编进 `ppet.exe`；
> - `tools/` —— 根目录的**测试与工具**（测试程序、素材生成、验收脚本），不进产品。
>
> 这是按「`src/` 下只允许 `ui` 与 `tools` 两个文件夹」的要求落地的结果：
> GUI 与逻辑分置 `ui` / `tools`，而验证工具留在根级，避免把 30 多个测试塞进产品源码树。

## 头文件根索引

全项目统一使用**从 workspace 根目录起算的尖括号索引**：

```cpp
#include <src/ui/DebugPanel.hpp>
#include <src/tools/core/Log.hpp>
#include <src/tools/model/Model.hpp>
#include <api/live2d/Live2DManifest.hpp>
#include <api/vendor/Json.hpp>
```

CMake 因此把 `CMAKE_CURRENT_SOURCE_DIR` 加入 include 路径（而不是 `src/`）：

```cmake
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)
```

**例外**：第三方/系统头保留库名前缀，不套 `src/` ——
`<nlohmann/json.hpp>`、`<stb/stb_image.h>`、`<GL/glew.h>`、`<GLFW/glfw3.h>`、`<QApplication>`。
它们不是本工程源码；`api/vendor/` 提供的是可选的统一引用头，见 [`api/README.md`](../api/README.md)。

生成物也遵循同一约定：文案翻译器输出到
`<build>/generated/src/tools/core/I18nStrings.inl`，代码里写
`#include <src/tools/core/I18nStrings.inl>`，把生成目录加进 include 路径即可。

## 为什么这样分

| 目标 | 做法 |
| --- | --- |
| GUI 与逻辑隔离 | `src/ui/` 只放 Qt 控件；`src/tools/` 不依赖 Qt（便于无头测试） |
| 专有 SDK 隔离 | Cubism SDK 的适配全在 `api/live2d/`，SDK 本体放 `api/live2d/csdk/` 并被 gitignore |
| 换第三方库不扩散 | `api/vendor/` 是单一引用点 |
| 路径不随文件位置漂移 | 根索引：移动文件时 include 只改前缀，不需要 `../` 相对路径 |
| 交叉编译友好 | `#include <src/...>` 只依赖一个 `-I <root>`，不受 `CMAKE_FIND_ROOT_PATH` 影响 |

## Live2D 动作系统分层

参考原 ppet / Touhou Desktop Pet 的动作组织（Idle 待机循环 + HitArea 点击互动 +
优先级打断 + Fade），拆成「解析 → 调度 → 驱动」三层，前两层与 Cubism SDK 解耦：

| 层 | 文件 | 职责 | 依赖 |
| --- | --- | --- | --- |
| 解析 | `Live2DManifest.*` / `Live2DMotion.*` | `model3.json` 的 Motions 分组、**HitAreas**、`motion3.json` 曲线与 **FadeIn/FadeOut** / **Meta.Loop** / 官方 `Segments` 语义 | 仅 json + FileIO |
| 动作队列 | `api/live2d/MotionQueue.hpp` | 官方 `MotionQueueManager` / `CubismMotion` 风格：多动作并存、`EasingSine` 交叉淡入淡出、`Meta.Loop`、`FinishPrimary` / `StopAll` | **纯 STL，零依赖** |
| 调度 | `MotionDirector.hpp` | 组名分类（Idle/Tap/Pinch…）+ **空名组按文件名回退推断**、空间分区命中、优先级状态机、待机冷却；内部持有动作队列 | **纯 STL，零依赖** |
| 物理 | `Live2DPhysics.*` | `physics3.json` 解析（标准 + Mobill 变体）、Verlet 摆锤链、固定步长与输入/输出插值 | 仅 json + FileIO |
| 渲染计划 | `api/live2d/CubismRenderPlan.hpp` | Drawable → `RenderOp` 决策层：renderOrder 稳定排序、三套 `glBlendFuncSeparate`、遮罩、premultiplied baseColor | **纯 STL，零依赖** |
| 驱动 | `Live2DModel.*` | 每帧复位参数默认值 → 叠加视线/眨眼/呼吸 → 按队列 `Blends()` 采样并 fade 混合写入 → 物理求值 → 口型；`Render()` 只执行 `RenderOp` | Cubism Core + GL |

> **纯 C++ 渲染设计**：动作队列与渲染计划这两层是按 `live2dwallpaper` 类纯 C++ 项目
> 与官方 `CubismNativeFramework` 的结构重写的（无 Chromium / 无 WebView），
> 把「决策」从 GL 代码里抽成可单测的纯逻辑，换渲染后端只需替换执行层。
> 详见 [Live2D 分层运行时 · 纯 C++ 渲染设计](live2d-stage.md#native-render)。

`IModel::OnInteract(nx, ny)` 是上层（App）点击 → 模型的统一入口，默认空实现，
PMX / VRM 不受影响。调度器的正确性由 `motion_director_unit`（状态机）、
`live2d_manifest_unit`（解析数据链）与 `live2d_motion_group_unit`（分组推断 / 循环 / Segments）
三个单测保证，三种 Cubism 配置都能跑。

> 解析层有个关键约束：`model3.json` 的 Motions 组值**标准写法是数组**
> （`"Idle": [ {"File": ...} ]`），旧实现只认对象/字符串，会把真实模型动作组解析成空。
> 见 `CollectMotionFiles()` 与 `live2d_manifest_unit` 的回归断言。

> **空名组必须能识别**：很多模型把全部动作塞进一个无名组 `""`，
> 只按组名分类会得到「无 Idle 组」→ 待机时零动作。
> 现在会先按组名分类，无语义组名再**按文件名**推断用途并抽出命中区，
> 见 `ClassifyMotionGroup()` / `ClassifyMotionFile()` / `HitAreaFromMotionFile()`。

> **`Segments` 必须按官方语义解析**：一条曲线 = 「第一个点 + 段标识」开头，
> 段标识后跟 1 个点（线性/阶跃）或 **3 个点**（贝塞尔 P1/P2/P3）。
> 详见 [Live2D 分层运行时](live2d-stage.md) 与官方规范
> `Live2D/CubismSpecs/FileFormats/motion3.json.md`。

## 表情与语言系统的分层

与动作系统同一思路：把「解析 → 调度 → 驱动」拆开，前两层做成**纯头文件**，
零 GL / 零 Qt / 零文件 IO，因此可以脱离渲染上下文做单元测试。

| 层 | 文件 | 职责 | 依赖 |
| --- | --- | --- | --- |
| 表情调度 | `src/tools/model/ExpressionDirector.hpp` | 预设 / 别名解析 / smoothstep 过渡 / 自动眨眼叠层 / 动作表情临时叠层 / 二元量化 / 表情时间轴 | **纯 STL，零依赖** |
| 语言系统 | `src/tools/agent/DialogueSystem.hpp` | 台词库（按状态分档）/ 气泡状态机（优先级·冷却·字数上限）/ 口型（音节包络→元音嘴形） | **纯 STL，零依赖** |
| Live2D 分层 | `api/live2d/Stage.hpp` | 参数分层栈（L0~L5）/ 姿态总线 / 三通道视线 / 待机导演 / 小表情 / 热区 / 口型导演 | **纯 STL，零依赖** |
| 驱动 | `PmxModel.*` / `VrmModel.*` / `Live2DModel.*` | 把调度结果刷进 morph 权重或 Cubism 参数 | GL / Cubism Core |

三种模型格式共用同一套语言层：`IModel` 上新增 `SetExpressionPreset` /
`SetExpressionWeights` / `SetLipSync` / `ExpressionPresetIds` 四个接口，
不支持的后端返回 `false`（默认实现），因此新增格式不会破坏上层。

> **口型不能覆盖表情**：口型是「叠在当前表情之上」的一层。
> PMX / VRM 后端的 `SetExpressionWeights` 写的是独立的口型叠加层，
> 而不是 `ExpressionDirector::PreviewWeights` —— 后者会清空全部基础权重，
> 每帧写口型都会把用户选的表情抹掉。

细节见 **[表情管理与语言功能](expressions.md)** 与
**[Live2D 分层运行时](live2d-stage.md)**。

## 待机动画的分层

待机表现拆成三套**互不依赖**的子系统，各自都是纯逻辑、可单测：

| 子系统 | 文件 | 适用格式 | 职责 |
| --- | --- | --- | --- |
| 程序化待机 | `src/tools/model/IdleAnimator.hpp` | PMX / VRM | 呼吸、重心转移、头部微摆、手臂摆动、重心转移与腿部抬起、眼睛微眯；频率互质避免节拍 |
| 骨骼动作 | `src/tools/model/BoneMotion.*` | PMX / VRM | 「骨骼名 + 欧拉角 + 权重 + 时间轴」的离散动作：点头 / 摇头 / 挥手；淡入淡出、播完回待机、待机随机触发（8~18 秒） |
| Live2D 物理 | `api/live2d/Live2DPhysics.*` | Live2D | `physics3.json` 驱动的摆锤链，头发与裙摆随头部转动甩动 |

三者都只输出「偏移量」，由各后端自行套用到骨骼或参数上，
因此可以脱离渲染层做单元测试：

| 单测 | 覆盖 |
| --- | --- |
| `idle_animator_unit` | 通道幅度、互质频率、小动作包络与触发间隔 |
| `bone_motion_unit` | 内置动作轨道、插值、淡入淡出、随机触发 |
| `live2d_physics_unit` | 两种 `physics3.json` 格式解析、摆锤链收敛、归一化映射 |

> 待机随机表情有**两条通道**：
> PMX / VRM 走 `ExpressionDirector` 的预设过渡，
> Live2D 优先走表情动作组，没有则回退到 `Stage.hpp` 的 `IdleFaces` 参数预设。
> 详见 [表情管理与语言功能](expressions.md)。

## 交互式控制台的分层

控制台要能操作模型、配置、Qt 气泡等**只有主线程能碰**的状态，因此拆成
「解析 → 传输 → 执行」三层，前两层完全不依赖 App：

| 层 | 文件 | 职责 | 依赖 |
| --- | --- | --- | --- |
| 解析 | `src/tools/app/ConsoleCommand.hpp` | 分词（引号 / 转义空格）、`Command`、`FormatTable`（`Idx Name Path`）、`HelpText` | **纯 STL，零依赖** |
| 传输 | `src/tools/app/Console.{hpp,cpp}` | 后台 stdin 线程把整行放进**有界**线程安全队列（上限 4096，超限计数 `DroppedLines()`）；`Poll()` 每帧最多取 32 条；`SubmitScript()` 脚本模式 | `std::thread` / `mutex` / `atomic` |
| 执行 | `src/tools/app/AppConsole.cpp` | 模型增删选列、`debug` 分节模型分析、`re` 重载、`action` / `expression`、`speak`、`status` | App 状态（**渲染主线程**） |
| 能力接口 | `src/tools/model/Model.hpp` | `MotionNames` / `PlayMotion` / `StopMotion` / `ReloadRenderer` / `RenderResources` / `ParameterSnapshot` 默认虚接口 | 无 |

关键约束：

- 后台线程只持有 `shared_ptr<State>`，**不捕获 `this`**；命令由 `App::ProcessInput()` 在主线程执行。
- `Stop()` 用 `detach()`，避免阻塞在 `std::getline` 时无法退出。
- `IModel` 用默认虚接口暴露能力，App 不需要按模型类型 downcast；后端不实现也能编译，
  控制台会明确提示「该后端不支持此操作」，而不是静默返回空结果。
- `--console` 打开交互模式；`--command='list;debug status;quit'` 用脚本模式做自动化验收
  （**成功指令只回显 stdout**，失败 / 用法错误 / 异常才写 DEBUG 级 `[CONSOLE]` 日志）。
- 命名与路径推导走 `src/tools/core/ModelNaming.hpp` 的纯字节函数，不调用 `std::filesystem`，
  因此 Windows 上遇到非法 UTF-8 字节路径不会让 `list` 整条抛异常。

指令手册见 **[使用与配置 · 交互式控制台](usage.md#交互式控制台)**。

## 本轮新增的基础设施

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 模型命名 | `src/tools/core/ModelNaming.hpp` | 纯字节切分推导显示名：默认名 = 文件名去后缀、重名加 ` (2)` 序号、URL 查询/片段与复合后缀处理；全程不访问磁盘、不做编码转换，对任意字节路径不抛异常 |
| 长期稳定性 | `src/tools/core/Stability.hpp` | `SaturatingIncrement()`（帧计数不回绕）、`SafeDelta()`、`ClampDelta()`（`dt` 对 NaN/Inf/负数/超大值收敛）、`PhaseOf()`（`double` 相位，30 年运行仍保精度） |
| 全局光标 | `src/tools/platform/GlobalCursorTracker.{hpp,cpp}` | 新增静态 `QueryCursor(Vec2&)`：Windows `GetCursorPos` / macOS `NSEvent.mouseLocation` / Linux `XQueryPointer`；点击穿透或追踪器未运行时由 `App::BuildRenderContext()` 兜底调用 |
| 动作过渡 | `src/tools/model/BoneMotion.{hpp,cpp}` | `Weight()` / `SetCrossFade()` / `SetEndFade()` / `HasOutgoing()` + 淡出层 `outgoing_`，`Accumulate()` 按骨骼名合并新旧姿态；一次性动作结束自动淡出 |
| 自然手臂 | `IModel::SetArmRestDrop()` / PMX·VRM 后端 | 给 A-pose / T-pose 上臂加静态 roll 放下胳膊（PMX 18°、VRM 55°），动作播放时按 `Weight()` 让位 |
| 帧宽高比 | `IModel::PreferredFrameAspect()` | PMX / VRM 取网格包围盒 X/Y，Live2D 取 `canvas.sizeInPixels`；`App::LoadModel()` 据此调整 GLFW 窗口，`feature_verify --auto-size` 复用 |
| 气泡自适应 | `src/ui/BubbleWidget.{hpp,cpp}` | `SetMaxWidth()` / `MaxWidth()` / `Width()` / `Height()`；最大宽度与字号跟随渲染窗口，相同宽度短路避免每帧重设样式 |
| 多行日志 | `src/tools/core/Log.{hpp,cpp}` | `SetMultiLine()` / `MultiLine()` / `FormatMultiLine()`；续行以 `> ` 开头且每行保留完整前缀，`Format()` 仍保持严格单行 |
| 无钩子光标轮询 | `src/tools/platform/GlobalCursorTracker.cpp` | Windows 分支移除 `WH_MOUSE_LL` 钩子及其钩子线程 / 消息循环，`Update()` 每帧 `GetCursorPos`；不再进入系统输入链 |
| 崩溃诊断 | `src/tools/platform/CrashHandler.{hpp,cpp}` | `SetUnhandledExceptionFilter` + `GetModuleHandleExW(FROM_ADDRESS)` 定位异常模块；纯逻辑 `ClassifyOrigin()` 区分自身 / 第三方注入 DLL，异常路径不用堆分配 |
| 限帧与兼容模式 | `src/tools/core/Stability.hpp` / `App::Run()` / `Window.cpp` | `EffectiveMaxFps()` / `FrameBudgetSeconds()` 软限帧 60 FPS；`--safe-mode` 降 30 并跳过帧读回；全屏检测节流到 250 ms |

新增单测：`model_naming_unit` / `stability_unit` / `console_queue_unit` /
`crash_classify_unit`，`bone_motion_unit` 另加 `TestCrossFade()` 与
`TestArmRotationGeometry()`（用 `Mat4::RotationZ().TransformDirection()` 验证下垂方向），
CTest 总数 39。

## 构建目标层次

```
ppet_zlib      (third_party/zlib)                 ─┐
ppet_model     (core/FileIO + model/* + render/*  │
                + api/live2d/* 若启用 SDK)         ├─> ppet (src/tools/main.cpp)
ppet_platform  (platform/* + GLFW/Qt/X11/Win32)   │      + src/ui/*
ppet_i18n_gen  (Compiler/，构建期生成 .inl)  ──────┘
```

`ppet_i18n_gen` 是宿主工具：凡编译了 `src/tools/core/I18n.cpp` 的目标，
CMake 末尾会**自动扫描并登记**依赖与生成目录，新增目标不会漏。

## 旧路径 → 新路径速查

| 旧（重构前） | 新 |
| --- | --- |
| `src/core/X` | `src/tools/core/X` |
| `src/model/X` | `src/tools/model/X` |
| `src/model/live2d/X` | `api/live2d/X` |
| `src/platform/X` | `src/tools/platform/X` |
| `src/render/X` | `src/tools/render/X` |
| `src/app/X` | `src/tools/app/X` |
| `src/main.cpp` | `src/tools/main.cpp` |
| `src/ui/X` | `src/ui/X`（不变） |
| `src/csdk/Core` | `api/live2d/csdk/Core`（旧位置仍兼容） |
| `#include "core/Log.hpp"` | `#include <src/tools/core/Log.hpp>` |

迁移用 `git mv` 完成，文件历史保留；CMake 里所有源文件路径与 `EXISTS` 判断已同步更新。
