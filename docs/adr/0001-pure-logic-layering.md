# ADR-0001：纯逻辑分层 —— 核心算法与 SDK/GUI 解耦

- 状态：已接受（Accepted）
- 日期：2026-11-21
- 相关：报告 4.2.4、`docs/architecture.md`

## 背景

桌宠涉及大量**可独立验证的规则**：动作分层仲裁、姿态叠加、注视判定、命中区映射、待机微动、骨骼动作、动作文件语义识别、追踪策略判定、数据解压预算。若这些规则直接写进渲染循环或 SDK 回调里，就只能靠"启动程序、肉眼看动画"来验证，无法单测，回归成本极高。

## 决策

把这些规则抽成**零图形依赖的纯函数/纯头文件**，集中在：

- `api/live2d/MotionDirector.hpp`（动作优先级与 fade）
- `api/live2d/Stage.hpp`（分层参数栈）
- `api/live2d/Live2DPhysics.*`（physics3.json 求解）
- `src/tools/model/IdleAnimator.hpp`、`BoneMotion.*`、`MotionRecognizer.*`
- `src/tools/core/TrackingPolicy.hpp`（Global/Window/None 判定）

它们只依赖标准库与项目内的值类型，可在没有 Cubism SDK、没有 OpenGL、没有窗口的环境里编译并单测（CTest 中的 `motion_director_unit` / `live2d_stage_unit` / `tracking_policy_unit` 等）。

## 理由

- **可测性**：规则一旦纯函数化，边界条件（NaN、负数、越界、循环回绕）都能写成断言，无需 GUI。
- **稳定性**：渲染/SDK 层被压薄成"取参数→驱动绘制"的适配壳，改动 SDK 版本不触碰规则。
- **三配置矩阵的基础**：正因为逻辑不依赖 SDK，`nocubism` 配置才能把 Live2D 后端整体剥离后仍然编译、测试通过。

## 后果

- 代价：多了一层值类型与接口，参数需要从 SDK 侧"搬运"进纯逻辑层再取回。
- 收益：Cubism SDK 升级、换渲染后端时，绝大多数已验证的规则无需重测。
