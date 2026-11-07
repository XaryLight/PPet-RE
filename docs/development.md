# 开发文档

← [返回 README](../README.md) · [文档索引](README.md)

---

## 项目结构

```
PPet-RE/
├── CMakeLists.txt            # 构建配置（C++23 / Qt6 / Cubism / 测试）
├── README.md                 # 项目主页（概览 + 快速开始 + 文档索引）
├── docs/                     # 分类文档（本目录）
├── src/                      # 产品源码：只有 ui / tools 两个文件夹
│   ├── ui/                   # ★ 全部 Qt GUI 实现
│   │   ├── SettingsPanel.*   #   设置窗口
│   │   ├── DebugPanel.*      #   模型诊断面板（6 页签）
│   │   └── ErrorDialog.*     #   模型加载失败/警告弹窗
│   └── tools/                # ★ 全部非 GUI 实现
│       ├── main.cpp          #   程序入口：QApplication → Logger::Init → App
│       ├── core/             #   Config / I18n / Log / FileIO / ZipArchive / ModelPackage / Math
│       ├── model/            #   IModel + ModelDebugInfo + ModelError、SkinnedMesh、pmx/ vrm/
│       ├── platform/         #   窗口、托盘、光标追踪、开机启动、文件对话框
│       ├── render/           #   Renderer / Camera / RenderUtils / FrameCapture
│       └── app/              #   App 主循环、托盘菜单、运行时状态采集
├── api/                      # ★ 外部 SDK / 第三方库引用层
│   ├── live2d/               #   Cubism SDK 集成（model3.json / moc3 / 动作 / Live2DModel）
│   │   └── csdk/             #   自备 Cubism Core（已 gitignore）
│   └── vendor/               #   Json.hpp / ImageIo.hpp / Compression.hpp 统一引用头
├── Compiler/
│   ├── i18n/                 # zh-CN.json / en-US.json（文案唯一来源）
│   └── i18n_gen/             # 文案翻译器源码与单元测试
├── third_party/              # nlohmann/json、stb、zlib（源码随仓库）
└── tools/                    # 测试程序、素材生成、render_verify.cpp、windows_verify.ps1
```

头文件统一用**根索引**（`#include <src/tools/core/Log.hpp>`），
CMake 把项目根目录加进 include 路径。完整约定见 [架构文档](architecture.md)。

## 架构分层

```
        ┌─────────────────────────────────────────────┐
        │  main.cpp（QApplication + Logger 初始化）      │
        └───────────────────┬─────────────────────────┘
                            │
        ┌───────────────────▼─────────────────────────┐
        │  App：主循环 / 配置 / 托盘 / 模型生命周期       │
        │   每帧：PollEvents → Update → Render →         │
        │        采集 RuntimeStats → DebugPanel.Update  │
        └───┬──────────┬──────────┬──────────┬────────┘
            │          │          │          │
       ┌────▼───┐ ┌────▼───┐ ┌────▼───┐ ┌───▼──────┐
       │  ui    │ │ model  │ │platform│ │  core    │
       │ Qt 6   │ │ PMX/VRM│ │ 窗口/  │ │ 日志/配置│
       │ 面板   │ │/Live2D │ │ 托盘   │ │ /ZIP     │
       └────────┘ └───┬────┘ └────────┘ └──────────┘
                      │
                 ┌────▼─────┐
                 │  render  │  统一蒙皮网格 + toon 着色
                 └──────────┘
```

**关键约定**

- 主窗口是 GLFW + OpenGL；Qt 只承载两个独立 `QDialog`，与 GLFW 共用**同一条主线程**，
  由 `App` 每帧调用 `PumpEvents()` 驱动 `QApplication::processEvents()`。
  两个面板都不是 `QObject`、不用 `Q_OBJECT`/moc，信号一律用 lambda 连接。
- 模型后端实现 `IModel`。`DebugInfo()` 与 `MissingResources()` 有**头文件内联默认实现**，
  新增后端不实现也不会导致链接错误（很多测试目标不编译 `Model.cpp`）。
- `Logger` 不依赖任何图形库，可被所有层使用；`Log.cpp` 用 `std::mutex` 保证整行原子写出，
  因此 CMake 全局链接了 `Threads::Threads`。
- ZIP 解压用 vendored zlib，不依赖系统 zlib。

---

## C++23 使用点

| 特性 | 位置 | 备注 |
| --- | --- | --- |
| `std::source_location` | `core/Log.*` | 日志宏自动带文件:行号:函数 |
| `std::string::contains` | `core/ModelPackage.cpp`、`model/Model.cpp` | 带 `#if __cpp_lib_string_contains` 回退 |
| `std::to_underlying` | `core/Log.cpp` | 带 `__cpp_lib_to_underlying` 回退 |
| `std::string_view::starts_with` | `src/tools/main.cpp` | 解析 `--log-level=` |
| `std::size` / 结构化绑定 / CTAD | 各处 | — |

CMake 要求 3.20+，配置阶段用
`if(NOT "cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)` 检查编译器能力，
不支持时给出中文 FATAL_ERROR。详见 [构建指南](build.md#环境要求)。

---

## 测试

```bash
cd build
ctest --output-on-failure
```

| 测试名 | 覆盖 |
| --- | --- |
| `platform_smoke` | 窗口 / 托盘 / 光标追踪的平台层冒烟（xvfb 虚拟显示） |
| `pmx_unit` | PMX 字段解析、UTF-8/UTF-16 文本、权重、骨骼层级、健壮性 |
| `vrm_unit` | glTF/VRM 解析与网格转换 |
| `log_unit` | 结构化日志：时间精度、字段完整性、级别阈值、UTF-8、多线程原子写出 |
| `i18n_gen_unit` | 文案翻译器：CSV/JSON 解析、成员解析、一致性校验、字面量转义、`.inl` 生成 |
| `i18n_gen_project` | 用仓库真实文案跑 `--check`，防止改了 JSON 忘重建 |
| `render_smoke` | 真实 GL 上下文下编译着色器、绘制网格、读回非零像素 |
| `render_integration` | 真实模型文件 → 后端 → 渲染层 → 截图 |
| `settings_panel_smoke` | Qt 设置面板创建 / 显示切换 / 事件泵（offscreen） |
| `debug_panel_smoke` | 模型诊断面板生命周期、伪造快照刷新、节流、语言切换、复制摘要、**表情与说话页内容** |
| `bubble_widget_smoke` | 台词气泡窗口：初始化幂等、文本读写、显隐开关、定位、可重复 Shutdown（offscreen） |
| `expression_director_unit` | 表情调度：预设 / 别名解析 / smoothstep 过渡 / 自动眨眼叠层 / 动作叠层淡出 / 二元量化 / 时间轴 |
| `dialogue_system_unit` | 语言系统：台词分档抽取 / 不重复 / 气泡优先级与冷却 / UTF-8 字数截断 / 口型音节包络与情绪嘴形 |
| `live2d_stage_unit` | Live2D 分层：参数栈层序仲裁 / 姿态总线 / 三通道视线 / 待机包络 / 小表情 / 热区 z 优先 / 口型导演 |
| `zip_package_test` | 生成测试用 zip（fixture） |
| `zip_import_test` | 解压中央目录、定位包内模型、幂等重导入、非法输入 |
| `pmx_package_gen` | 生成 4 种 PMX 包（fixture）：完整 / 缺贴图 / 大小写 / 子目录 |
| `pmx_package_test` | PMX 包统计、缺失贴图诊断、贴图大小写与目录容错 |
| `unicode_path_test` | 中文路径读写、目录遍历、ZIP 条目名编码归一化 |
| `model_error_unit` | 结构化错误：分类名/建议、真实错误串分类推断、明细与报告组装 |
| `error_dialog_smoke` | 失败弹窗：分类/摘要/逐条明细/建议/日志、复制按钮、警告型标题（offscreen） |
| `render_verify_pmx` / `render_verify_vrm` | 用渲染验证 CLI 对仓库素材跑「加载 → 渲染 → 读回像素 → 判定」（xvfb） |

共 **28 项**测试（含 fixture）。调试面板冒烟测试还会校验 7 个页签、骨架/图片/详细错误区与
表情与说话页；气泡冒烟测试校验无边框气泡的显隐、文本与定位。

渲染验证 CLI（可单独用于人工核验或 CI）：

```bash
./build/ppet_render_verify --model <.vrm|.pmx|.zip|.model3.json|.moc3> \
    --shot shot.png --report report.json --frames 30
# Windows 11 真机一键验收见 docs/windows-test.md 与 tools/windows_verify.ps1
```

测试素材由脚本生成（CMake 会自动作为 fixture 调用）：

```bash
python3 tools/gen_test_pmx.py         # 最小合法 PMX + 贴图
python3 tools/gen_test_vrm.py         # 最小 VRM
python3 tools/gen_test_zip.py <path>  # 通用模型包
python3 tools/gen_test_pmx_zip.py <目录>  # 4 种 PMX 包
```

---

## 界面文案（Compiler/）

界面文案不在 C++ 代码里，而在 `Compiler/i18n/{zh-CN,en-US}.json`（也可用 CSV）。
构建时由 **文案翻译器**（`Compiler/i18n_gen/`）翻译成 C++ 内嵌表
（`constexpr` 表 + X 宏，生成到构建目录，运行时零成本）。

| 特点 | 说明 |
| --- | --- |
| 单一来源 | 改文案只改 JSON，不用在两个 `if` 分支里手抄两遍 |
| 构建期校验 | 「文案键 ↔ `struct Strings` 成员」必须一一对应，少一条 / 多一条 / 拼错键都会立刻报错 |
| 支持 CSV | 按扩展名自动判断 `.json` / `.csv`；含逗号的字段用双引号包起来即可 |
| 可离线校验 | `ppet_i18n_gen --check` 只比对不写文件，CI 可用来检查生成物是否过期 |

```bash
# 改完文案后直接重新构建，CMake 会自动重跑翻译器
cmake --build build

# 用 CSV 作为文案源
cmake -S . -B build \
  -DPPET_I18N_ZH=Compiler/i18n/zh-CN.csv \
  -DPPET_I18N_EN=Compiler/i18n/en-US.csv
```

> 新增文案需要同时改三处：`Compiler/i18n/zh-CN.json`、`en-US.json`、`src/tools/core/I18n.hpp` 的
> `struct Strings`。`src/tools/core/I18n.cpp` 用 X 宏赋值，**不需要改**。
> CMake 会扫描所有目标，凡是编译了 `I18n.cpp` 的都会自动加上生成依赖与头文件路径。

详见 [Compiler/README.md](../Compiler/README.md)。

---

## 新增模型后端时要做什么

1. 实现 `IModel`（`Load` / `Update` / `Render` / `OnResize` / `DisplayName` / `Type` / `IsLoaded`）；
2. 在 `CreateModel()` 注册；
3. 建议重写 `DebugInfo()`（先调 `FillMeshDebugInfo()` 填规模，再补格式特有的性质）
   与 `MissingResources()`（有外部资源时）；
4. 在 `tools/` 下加对应测试并注册到 `CMakeLists.txt`。

诊断面板与日志对后端是**解耦**的：不重写 `DebugInfo()` 只显示概览，不会崩溃。

---

## 相关文档

- [项目结构与头文件索引](architecture.md) · [构建指南](build.md) · [Windows 真机验收](windows-test.md)
- [模型格式与导入](models.md) · [使用与配置](usage.md) · [调试与日志](debugging.md)
- [api/ 集成层说明](../api/README.md) · [文案翻译器](../Compiler/README.md)
