# ADR-0002：Cubism SDK 单点隔离

- 状态：已接受（Accepted）
- 日期：2026-11-21
- 相关：报告 4.2.4、`api/README.md`、`docs/build.md`

## 背景

Live2D Cubism SDK for Native 是闭源、需授权、按平台提供二进制（`.lib`/`.a`/`.dll`）的依赖，且不同版本 API 有差异。若 SDK 类型（如 `Csm::CubismModel`）散落在产品代码各处，会产生两个问题：

1. 没有 SDK 的用户无法编译任何涉及 Live2D 的目标；
2. SDK 升级导致修改点遍布全工程，回归面巨大。

## 决策

把所有对 Cubism Core 的直接调用收敛到**唯一适配点** `api/live2d/CubismCore*`，其余 Live2D 代码只面向项目自己的值类型与接口。构建层面提供三种后端选择：

- `PPET_WITH_CUBISM=ON`：真实 SDK（用户自备，放 `api/live2d/csdk/`）；
- `PPET_WITH_CUBISM_STUB=ON`：仓库内置 Core 桩，验证构建链路与 UI，不加载真实模型；
- 两者皆关：`Live2D` 后端整体可选，加载时给出友好提示。

SDK 头文件通过 CMake 的条件编译宏（而非无条件 `#include`）引入。

## 理由

- **可移植**：无 SDK 环境（多数贡献者、CI）用桩即可开发与跑测试。
- **可升级**：SDK 变更只影响适配点一层。
- **诚实交付**：桩不会伪装成"能加载真实模型"，避免误导用户。

## 后果

- 代价：适配层需要维护一套与 SDK 对齐的接口，桩与真实实现需保持行为一致（这也是 fuzz/契约测试关注点）。
- 收益：实现了"三配置矩阵"这一核心工程能力，见 ADR-0003。
