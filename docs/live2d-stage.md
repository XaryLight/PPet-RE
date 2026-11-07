# Live2D 分层运行时

Live2D 角色的「活」来自十几个参数同时被驱动：视线、待机微摆、小表情、说话口型、
交互姿态……如果它们各自直接写参数，后写的一定会覆盖先写的，动作之间会互相打架。

本文说明 PPet 采用的**分层参数仲裁**方案，源码在 `api/live2d/Stage.hpp`。
该文件是**纯头文件**（零 GL / 零 Cubism SDK 依赖），可脱离 SDK 单元测试
（`ppet_live2d_stage_test`）。

参考来源：[FULASKA/-live2d-](https://github.com/FULASKA/-live2d-) 的 `SPEC` 中
「分层参数仲裁 / 姿态总线 / 三通道视线 / IdleDirector / IdleFaces / RegionSet /
MouthDirector」这套组织方式。

---

## 一、分层参数栈（ParameterStack）

每帧**从默认姿态重建**，再按层序写入。层号越大优先级越高：

> 本节描述的是 `Stage.hpp` 提供的能力。其中 `ParameterStack` / `PoseBus` / `IdleFaces` /
> `RegionSet` 四个组件**已实现并有单元测试**，但尚未接入 `Live2DModel`
> （当前接线状态见 [第七节](#七当前接线状态)）；`GazeChannels` / `IdleDirector` /
> `MouthDirector` 已在用。

| 层 | 枚举 | 写入模式 | 谁在写 |
| --- | --- | --- | --- |
| L0 | — | 基线 | 模型默认参数值 |
| L1 | `ParamLayer::Gaze` | Override | 视线追踪（眼 / 头 / 身体） |
| L2 | `ParamLayer::Pose` | Add | 姿态总线（手势造成的临时偏移） |
| L3 | `ParamLayer::Idle` | Override | 待机微摆 + 随机成套动作 |
| L4 | `ParamLayer::Face` | Add | 小表情（随机点缀 / wink） |
| L5 | `ParamLayer::Force` | Override | 强制互动（用户显式触发） |

写入模式有两种：

- `WriteMode::Override` —— 同层后者覆盖前者，高层覆盖低层；
- `WriteMode::Add` —— 累加（姿态总线与小表情用，避免互相抵消）。

```cpp
ParameterStack stack;
stack.Reset(defaults);                                   // 每帧开始
stack.Write(ParamLayer::Gaze,  "ParamAngleX",  12.0f);   // L1 覆盖
stack.Write(ParamLayer::Pose, "ParamAngleX",   3.0f, WriteMode::Add);
stack.Write(ParamLayer::Force, "ParamAngleX", -8.0f);     // L5 最终胜出

const auto& out = stack.Commit();   // "参数名 -> 最终值"
```

退出某层时该层的贡献被清空，参数自然回落到低层，不会残留「上一次手势的偏移」。

---

## 二、姿态总线（PoseBus）

手势（摸头、拖拽、点击）产生的偏移是**加性**的：多个手势可以叠加，
也可以互相抵消。`PoseBus` 收集本帧的增量，在物理计算**前后**各 `Flush()` 一次：

```cpp
bus.Add("ParamAngleX", 3.0f);
bus.Add("ParamAngleX", -1.0f);   // 净额 +2.0
const auto delta = bus.Flush();  // 取出并清空
```

---

## 三、三通道视线（GazeChannels）

眼睛、头、身体**分开驱动**，速度递减，形成自然的「眼先动、头跟上、身体最后轻微转」：

| 通道 | 默认速度 | 说明 |
| --- | --- | --- |
| 眼 | 20.0 /s | 眼球最快，先锁定目标 |
| 头 | 7.0 /s | 头跟随，约慢一半 |
| 身体 | 3.5 /s | 身体只做轻微转动（目标值取 `target * 0.5`） |

- **指数阻尼**：`DampTo(current, target, dt, speed)`，帧率无关；
- **非对称增益**：回中心的速度（1.6）比偏离中心（1.0）快 —— 视线「收回」比「转开」更利落；
- 没有鼠标时传入 0，三个通道平滑回中。

---

## 四、待机系统（IdleDirector + IdleFaces）

### IdleDirector —— 微摆 + 随机成套动作

| 组成 | 默认值 |
| --- | --- |
| 微摆 | 偏航 ±0.0262 rad、俯仰 ±0.0131 rad、呼吸 ±0.05 |
| 呼吸 | 1.6 Hz |
| 随机动作间隔 | 基准 8 s ± 10 s 抖动（即约 8~18 s） |
| 动作包络 | **钟形**（起承转合），中段最强、两端归零 |

`Tick(dt, talkMask)` 的 `talkMask` 是「说话/互动时压低待机微摆」的程度 ——
避免桌宠「边说话边大幅晃动」。

### IdleFaces —— 随机小表情

在候选小表情里随机挑一个短暂叠加（冷却默认 5~6 s），另外支持 `TriggerWink()`
主动触发一次眨眼（配合 `blinkLeft` / `blinkRight` 通道做出「眨一只眼」的俏皮表情）。

---

## 五、交互热区（RegionSet）

多边形热区，按 **z 值从大到小**做命中测试：

```cpp
RegionSet regions;
// { 名称, 归一化多边形顶点, z 优先级(越大越先命中), 手势类型 }
regions.Add(Region{"head", {{0.3f, 0.1f}, {0.7f, 0.1f}, {0.7f, 0.4f}, {0.3f, 0.4f}}, 2, "pet"});
const std::string hit = regions.Hit(nx, ny);   // 返回命中的区域名
```

每个区域带一个手势类型（`tap` / `pet` / `drag` …），
`GestureOf(name)` 直接取出。

**长期态**：某区域被持续触发超过阈值（默认 2 s）进入「长期态」，
可用于「一直摸头就进入特殊反应」这类设计。
只有当前 active 区域累加时间，其它区域清零 —— 松手后长期态自然解除。

---

## 六、口型（MouthDirector）

Live2D 的口型由 `MouthDirector` 统一驱动：

- **呼吸微动**：静止时 `ParamMouthOpenY` 也有轻微起伏（幅度 0.03）；
- **说话**：`Tick(dt, talkOpen)` 中的 `talkOpen` 来自 `LipSync`（见
  [表情管理与语言功能](expressions.md#23-口型mouth)），
  目标值 = 呼吸 + 说话开口度，再**平滑追向**目标，避免参数抖动；
- **嘴形**：`SetForm(form)` 写入 `ParamMouthForm`（笑为正、怒为负）；
- **量程**：`SetRange(minOpen, maxOpen)` 可限定开口度范围（例如 0.15 ~ 0.9，
  让嘴不会张到极限）。

---

## 七、当前接线状态

`Stage.hpp` 提供 7 个组件，**全部有单元测试覆盖**；`Live2DModel` 目前接入了其中 3 个：

| 组件 | 是否已接入 Live2DModel | 说明 |
| --- | --- | --- |
| `GazeChannels` | ✅ 已接入 | 替代原先直接写参数的做法，眼/头/身体分别阻尼 |
| `IdleDirector` | ✅ 已接入 | 待机微摆 + 随机成套动作，说话时按 `talkMask` 让位 |
| `MouthDirector` | ✅ 已接入 | `ParamMouthOpenY` / `ParamMouthForm`，每帧平滑追向目标 |
| `ParameterStack` | ⬜ 已实现未接入 | 分层仲裁就绪，可用于把动作组与上述三层统一仲裁 |
| `PoseBus` | ⬜ 已实现未接入 | 姿态增量总线，等接入手势（拖拽 / 抚摸）时启用 |
| `IdleFaces` | ⬜ 已实现未接入 | 随机小表情 + wink |
| `RegionSet` | ⬜ 已实现未接入 | 多边形热区（当前互动沿用 Cubism `HitAreas`） |

已接入部分的每帧流程：

```
每帧：
  gaze_.Tick(dt, targetX, targetY)   → 眼/头/身体角度
  idleStage_.Tick(dt, talkMask)       → 待机微摆（说话/互动时压低）
  mouthStage_.Tick(dt, talkOpen)      → ParamMouthOpenY / ParamMouthForm
  → 刷进 Cubism 参数
```

待接入的分层仲裁一旦启用，接线会变成：

```
每帧：
  stack.Reset(defaults)                 // L0 基线
  gaze.Tick(dt, targetX, targetY)       // L1
  idle.Tick(dt, talkMask)               // L3
  idleFaces.Tick(dt, cooldown)          // L4
  bus.Flush() → stack.Write(Pose, ...)  // L2（物理前）
  ... 物理 / 弹簧骨 ...
  bus.Flush() → stack.Write(Pose, ...)  // L2（物理后）
  stack.Commit() → 刷 Cubism 参数
```

---

## 八、相关源码

| 文件 | 职责 |
| --- | --- |
| `api/live2d/Stage.hpp` | 分层参数栈 / 姿态总线 / 三通道视线 / IdleDirector / IdleFaces / RegionSet / MouthDirector |
| `api/live2d/Live2DModel.cpp` | 把上面的输出刷进 Cubism 参数（`ParamAngleX` / `ParamEyeOpenX` / `ParamMouthOpenY` …） |
| `api/live2d/MotionDirector.hpp` | 动作组调度（待机 / 互动 / 表情），与 Stage 互补 |

单元测试：`tools/live2d_stage_test.cpp`（28 项检查）。

相关文档：[表情管理与语言功能](expressions.md) · [使用与配置](usage.md) ·
[项目结构](architecture.md)
