# 开发文档

← [返回 README](../README.md) · [文档索引](README.md)

---

## 项目结构

```
PPet-RE/
├── CMakeLists.txt            # 构建配置（C++23、模型层 / 平台层 / 主程序 / 测试）
├── README.md                 # 项目主页（概览 + 快速开始 + 文档索引）
├── docs/                     # 分类文档（本目录）
├── src/
│   ├── main.cpp              # 程序入口：QApplication → Logger::Init → App
│   ├── core/
│   │   ├── Config.*          # 配置持久化（JSON，字段缺失回退默认值）
│   │   ├── I18n.*            # 界面文案结构（值由翻译器生成的 .inl 填充）
│   │   ├── Log.*             # 结构化日志（毫秒时间戳 / source_location / 双阈值 / 互斥写）
│   │   ├── FileIO.*          # 跨平台 UTF-8 路径与文件读写
│   │   ├── ZipArchive.*      # ZIP 中央目录解析 + zlib inflate 解压
│   │   ├── ModelPackage.*    # 模型包导入、内容统计、包内模型定位
│   │   └── Math.hpp          # Vec2/3/4、Mat4
│   ├── model/
│   │   ├── Model.*           # IModel 接口、ModelDebugInfo、模型工厂、类型探测
│   │   ├── SkinnedMesh.*     # 统一蒙皮网格表示
│   │   ├── pmx/              # PMX 2.0/2.1 解析、转换为网格、贴图路径容错
│   │   ├── vrm/              # GLB/glTF + VRM 扩展解析
│   │   └── live2d/           # Live2D Cubism 后端（可选）
│   ├── render/               # toon 着色渲染管线、相机、GL 错误检查
│   ├── platform/             # 窗口、托盘、全局光标追踪、开机启动、文件对话框
│   ├── ui/
│   │   ├── SettingsPanel.*   # Qt 6 设置面板
│   │   └── DebugPanel.*      # Qt 6 模型诊断面板
│   └── app/App.*             # 主循环、配置/模型/托盘编排、运行时状态采集
├── src/csdk/                 # Cubism SDK Core（可选，需自备，已 gitignore）
├── Compiler/
│   ├── i18n/                 # zh-CN.json / en-US.json（文案唯一来源）
│   └── i18n_gen/             # 文案翻译器源码与单元测试
├── third_party/              # nlohmann/json、stb、zlib（源码随仓库）
└── tools/                    # 测试程序与测试素材生成脚本
```

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
| `std::string_view::starts_with` | `src/main.cpp` | 解析 `--log-level=` |
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
| `debug_panel_smoke` | 模型诊断面板生命周期、伪造快照刷新、节流、语言切换、复制摘要 |
| `zip_package_test` | 生成测试用 zip（fixture） |
| `zip_import_test` | 解压中央目录、定位包内模型、幂等重导入、非法输入 |
| `pmx_package_gen` | 生成 4 种 PMX 包（fixture）：完整 / 缺贴图 / 大小写 / 子目录 |
| `pmx_package_test` | PMX 包统计、缺失贴图诊断、贴图大小写与目录容错 |
| `unicode_path_test` | 中文路径读写、目录遍历、ZIP 条目名编码归一化 |

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

> 新增文案需要同时改三处：`Compiler/i18n/zh-CN.json`、`en-US.json`、`src/core/I18n.hpp` 的
> `struct Strings`。`src/core/I18n.cpp` 用 X 宏赋值，**不需要改**。
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

- [构建指南](build.md) · [模型格式与导入](models.md) · [使用与配置](usage.md) · [调试与日志](debugging.md)
