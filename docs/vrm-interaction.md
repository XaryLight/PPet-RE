# VRM 动作交互：凝视自然化与参考项目借鉴

← [返回 README](../README.md) · [文档索引](README.md)

---

## 背景：三个桌宠参考项目

本轮以三个开源桌宠为参照，学习它们的 **VRM 模型渲染 / 动作交互** 设计：

| 项目 | 技术栈 | 最值得借鉴的点 |
| --- | --- | --- |
| [xiaomate-desktop](https://github.com/oddmeta/xiaomate-desktop) | Electron + PIXI.js(Live2D) + Three.js(VRM) | 站立桌宠的点击分区命中（`hit` -> `TapBody`）、9 组 Idle 轮播、走动动画名探测（`Walk`/`walk`/`WALK`…）、透明区域点击穿透。VRM 在发行包里，主分支为 Live2D。 |
| [dsh-plugin-vrmpet3d](https://github.com/drliuhuan/dsh-plugin-vrmpet3d) | Three.js + `@pixiv/three-vrm` | **工程化最成熟的凝视系统**：均匀 ramp 起停、鼠标静止休眠、最后方向记忆、头部预算让位、全程 NaN 防御；`authored` clip 播放时程序化姿态让位；弹簧骨固定步长。 |
| [STVrmPet](https://github.com/kencuo/STVrmPet) | Three.js（SillyTavern 扩展） | 程序化走路步态（手臂摆动 + 膝盖弯曲 + **膝轴自动推断**）、头/颈/胸/躯干**分层视线权重**（0.52/0.38/0.12/0.10）、摸头反应 + 爱心粒子、动作互斥与冷却。 |

PPet 已有的能力（分区触摸反馈 `TouchResponse`、内置骨骼动作 nod/shake/wave、动作交叉淡化 `BoneMotionPlayer`、自动眨眼、弹簧骨、程序化待机）覆盖了大部分交互面。**本轮落地的是 PPet 尚缺的「凝视自然化」**——它直接决定「鼠标看过来」这一最高频交互的观感。

---

## 凝视自然化（`src/tools/model/GazeFollow.hpp`）

一个纯逻辑模块（零 GL / Qt / 文件 IO，`GazeFollow` 可在无图形环境下单测），把「光标目标角度」变成「可直接写进骨骼的有效角度」。综合 dsh 与 STVrmPet 的成熟做法：

### 1. 目标指数平滑（STVrmPet）

```
aim += (target - aim) * (1 - exp(-k·dt))     // k 默认 9，与旧实现一致
```

避免快速甩鼠标时头部瞬移。慢速移动照样跟（平滑只作用于角度，不冻结目标）。

### 2. 均匀 ramp 起停（dsh）

```
gazeBlend += clamp(target - gazeBlend, -dt/T, +dt/T)   // T = blendSeconds
```

凝视不是开关：光标出现 / 离开时，跟随强度在 T 秒内**匀速**升降（默认 1 秒），不再瞬间盯上或瞬间回正。这是「自然过渡」的核心。

### 3. 鼠标静止休眠（dsh）

帧间目标变化小于 `epsilon`（默认 0.002 ≈ 0.11°）时不刷新计时；连续 `idleSeconds`（默认 6 秒）无实质移动就停止凝视，下次移动立即恢复。避免盯着一个不动的光标发呆。

### 4. 最后方向记忆（dsh）

休眠 / 清目标后，凝视沿「最后一次有效方向」淡出，而不是无方向地跳回 0。

### 5. 头部预算让位（dsh `HEAD_LAYER_LIMIT`）

内置动作（点头 / 摇头 / 挥手）也在转头时，把动作已占用的头部转角喂进来：

```
yaw = clamp(yaw, ±max(0, headBudget - motionHeadTurn))
```

凝视只用剩余预算，避免两套系统抢同一根骨骼导致抽搐 / 过冲。VRM 后端在 `ApplyBoneMotion` 里统计 head 通道的合角度并注入。

### 6. 有限值防御（dsh）

NaN / Inf 一旦写进骨骼四元数，模型会在两帧后整个消失。每个输入与输出都做 `std::isfinite` 检查，坏值一律回退到安全值；`dt > 0.1` 钳到 0.1（后台标签页恢复保护）。

---

## 后端接线

- `VrmModel::Load()`：`gazeFollow_.Reset()` 并设 `weight = 1.0`（Q5「最大偏转角」滑块与最终头部转角一一对应；lookAt 上限已在 `ResolveGazeLimits` 钳过）。
- `VrmModel::UpdateGaze()`：喂入 `SetAim` / `ClearAim` 与动作头部转角，用 `Tick(dt)` 的输出驱动头部与眼球骨骼，取代旧的手写指数平滑。
- 暴露 `SetGazeBlendSeconds` / `SetGazeIdleSeconds` / `GazeBlend()` 供设置面板与测试调节。

PMX 后端（`PmxModel`）仍用原指数平滑，未改动——本轮聚焦 VRM 交互。

---

## 测试

- `tools/gaze_follow_test.cpp`：25 项断言，覆盖平滑、ramp、休眠、epsilon、头部预算、NaN 防御、复位。
- `tools/vrm_test.cpp::TestGazeNaturalization`：5 项集成断言，验证接线（渐入、休眠回落、恢复、重新加载复位）。VRM 测试总数 232 -> 237。

```bash
./build-stub/ppet_gaze_follow_test   # 25/25
./build-stub/ppet_vrm_test           # 237/237
```

---

## 暂未纳入参考项目的其它能力

- **走路步态 / 膝轴推断（STVrmPet）**：PPet 桌宠是站姿、窗口由用户拖动，没有「桌面走动」语义，程序化走路暂不适用。
- **分层视线权重（STVrmPet）**：PPet 只驱动 head + 眼球（无 neck / chest 独立骨骼绑定时优雅降级），分层收益有限，留待后续。
- **VRMA / FBX 动画（xiaomate）**：涉及新动画格式解析与版权素材，超出本轮范围。
