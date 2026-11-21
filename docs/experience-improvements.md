# 第四部分「项目体验改进」落地清单

> 对应 `PPet-RE-项目深度分析报告.md.txt` 第四部分（4.1 终端用户体验 / 4.2 开发者体验）。
> 本页记录每一项的落地状态与取舍理由，供后续贡献者接力。
> 原则：**风险可控、可验证、每批完成即提交**；重型/高成本项不擅自引入。

## 状态总览

| 编号 | 改进项 | 状态 | 落地位置 |
| --- | --- | --- | --- |
| 4.1.1 | AI 对话 / TTS / 屏幕感知 / 进程隔离 | ⏸ 未引入 | 重型架构（LLM/TTS/ZeroMQ），需独立需求评估 |
| 4.1.2 | 模型导入向导 / 模型市场 / 格式场景化 | ⏸ 未引入 | 涉及 GUI 向导与外链资源，另行规划 |
| 4.1.3 | 分场景自适应帧率与节能 | ✅ 已落地（保守版） | `src/tools/core/AdaptiveFps.hpp`、`App.cpp` |
| 4.1.4 | 拖拽惯性/吸附、点击粒子、全局快捷键、无障碍 | ⏸ 未引入 | 跨 GUI / 平台，风险高，另行规划 |
| 4.1.5 | 本地化：硬编码收口 + `--lang` + 系统语言检测 | ✅ 已落地 | `LanguageDetect.*`、`main.cpp`、`App.cpp`、`ModelNaming.hpp`、i18n JSON |
| 4.2.1 | CMake Presets + 依赖管理 + 告警区分 | ✅ 大部分落地 | `CMakePresets.json`、`CMakeLists.txt`（`/W3` vs `-Wall -Wextra`） |
| 4.2.2 | 错误引导 + 日志轮转 | ✅ 已落地 | 轮转早已实现（4 MiB→`.1`）；`--doctor` 给出修复建议 |
| 4.2.3 | `--doctor` 自检 + 运行统计导出 | ✅ 体检落地 | `Doctor.*`、`main.cpp`；JSON/CSV 统计导出未做（低优先） |
| 4.2.4 | 文档 ADR + 插件完整示例 | ✅ ADR 落地 | `docs/adr/0001~0003`；插件示例仍为内置计数器/问候 |
| 4.2.5 | CI/CD 矩阵 + 发布自动化 | ✅ CI 落地 | `.github/workflows/ci.yml`；tag 自动发布未做 |

## 落地细节

### 4.1.3 分场景自适应帧率（保守版）

`AdaptiveFps.hpp` 是纯函数：`EffectiveAdaptiveFps(configured, safeMode, scene, enabled)`。
场景只依赖可靠的 `GLFW_VISIBLE`：窗口隐藏/最小化 → 1 FPS 心跳，其余完全沿用既有配置。
默认开启，可用 `--no-adaptive-fps` 关闭。

**为何不做「窥屏/失焦降到 15 FPS」**：桌宠窗口常为 toolwindow/NOACTIVATE，`GLFW_FOCUSED`
几乎恒为 false，据此降频会让动画常态性卡顿；可靠的遮挡检测（Windows `DWMWA_CLOAKED`）
跨平台语义不一，误判代价高。故默认只做「明确不可见」这一档，把激进阶留作可选扩展。

### 4.1.5 本地化

- `--lang=zh|en`（及 `zh-CN` / `en-US` / `chinese` / `english`）运行时覆盖界面语言，不写回配置。
- 系统语言检测：优先 `LC_ALL`/`LC_MESSAGES`/`LANG`，再退回 Windows `GetUserPreferredUILanguages`
  / macOS `CFLocaleCopyPreferredLanguages`；仅**首次启动**生效，之后尊重用户托盘选择。
- `ModelNaming` 的「模型 N」兜底改为可注入前缀（`ResolveNames`/`NameAt` 可选参数，默认保持兼容），
  文案 `modelNamePrefix` 纳入 i18n 单一来源，GUI 改名路径注入当前语言前缀。
- 仍有少量控制台/GUI 硬编码（如 QInputDialog 标题、控制台英文提示）未全量收口，属渐进工作。

### 4.2.1 构建

- `CMakePresets.json`（v2，兼容 CMake 3.20）封装 stub/cubism/nocubism/asan 四配置。
- 告警开关按编译器区分：MSVC `/W3`、GCC/Clang `-Wall -Wextra`，避免第三方头告警洪水。
- vcpkg/Conan 依赖管理未引入：项目 vendored zlib/stb 已能离线构建，引入包管理器会改变
  贡献者工作流且收益有限，暂缓。

### 4.2.3 / 4.2.2 体检与日志

`ppet --doctor` 无 GUI 依赖，输出后端/配置/模型/日志四项报告，退出码 0/2 便于脚本使用。
日志 4 MiB 轮转早已实现并经 `log_unit` 覆盖；体检命令补充了「配置越界已被自动修正」等
过去被静默钳制而不可见的信息。

### 4.2.4 / 4.2.5 文档与 CI

新增 3 份 ADR 解释关键设计「为什么」（纯逻辑分层 / SDK 单点隔离 / 三配置矩阵）。
CI 矩阵跑 stub/nocubism/asan（真实 SDK 因许可证无法在公开 CI 分发），ASAN 用 `ctest -E`
排除两个已知 Qt GUI 内存泄漏用例（见 `docs/debugging.md`，属存量问题）。

## 有意未引入 / 延后

- AI/TTS、进程隔离 ZeroMQ、模型市场、导入向导、全局快捷键、点击粒子：均为中高成本功能，
  需要独立需求与设计，不在「依据报告逐条修复」的健壮性完善批次内强行实现。
- 插件完整示例：现有 `BuiltinPlugins.hpp` 的计数器/问候已演示扩展点，完整业务插件另行规划。
- tag 自动发布 / RuntimeStats 的 JSON-CSV 导出：低优先，待 CI 稳定运行后补充。
