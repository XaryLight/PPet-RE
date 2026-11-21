# ADR-0003：三配置构建矩阵 + CMake Presets

- 状态：已接受（Accepted）
- 日期：2026-11-21
- 相关：报告 4.2.1 / 4.2.5、`docs/development.md`、`CMakePresets.json`、`.github/workflows/ci.yml`

## 背景

项目同时要满足三个互相冲突的目标：

1. 贡献者无 Cubism SDK 也要能构建、跑测试；
2. 维护者要用真实 SDK 验收 Live2D 模型；
3. 需要证明"Live2D 后端是真正可选的"，而不是编译期强耦合。

历史上这三件事靠口头约定和手写 `cmake -D...` 命令完成，新人容易配错，CI 也无法复现。

## 决策

固化为三个命名配置（外加一个 sanitizer 配置）：

| 配置 | 关键开关 | 用途 |
| --- | --- | --- |
| `stub`（Release） | `PPET_WITH_CUBISM_STUB=ON` | 默认开发配置 |
| `cubism`（Release） | `PPET_WITH_CUBISM=ON` | 真实 SDK 验收 |
| `nocubism`（Debug） | 两者皆 `OFF` | 验证后端可选 |
| `asan`（Debug） | 桩 + `-fsanitize=address,undefined` | 健壮性回归 |

新增 `CMakePresets.json`（`cmake --preset stub` 一条命令复现）与 `.github/workflows/ci.yml`（CI 矩阵跑 stub/nocubism/asan）。真实 SDK 配置因许可证无法在公开 CI 分发，由维护者本地或自建 runner 验收。

告警开关按编译器区分（MSVC 用 `/W3`，GCC/Clang 用 `-Wall -Wextra`），避免第三方头告警洪水掩盖真实问题。

## 理由

- **可复现**：新人一条 preset 命令得到与 CI 完全一致的构建。
- **可验证**：三配置同时绿，本身就是"后端可选 + 逻辑分层"的回归证明。

## 后果

- 代价：CMakePresets v2 不支持用例过滤，ASAN 的已知 Qt GUI 泄漏项在 CI 里用 `ctest -E` 排除。
- 收益：贡献门槛显著降低，回归覆盖可自动执行。
