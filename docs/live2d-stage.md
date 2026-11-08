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

### IdleFaces —— 随机小表情（已接入）

Live2D 的「表情」有两种载体，本项目**两条都支持**：

| 载体 | 说明 | 接入方式 |
| --- | --- | --- |
| 表情动作组 | `model3.json` 的 `Motions` 里名字含 `Exp` 的组 | `MotionDirector` 播放 |
| **表情参数** | `ParamBrowLY` / `ParamCheek` / `ParamEyeLSmile` 等 | `IdleFaces` 加性叠加 |

**为什么必须有第二条通道**：很多模型（尤其是国内常见的一体成型模型包）
**没有任何表情动作组**，只有几十个表情参数。只做第一条通道，
「待机表情随机化」对这些模型就是完全无效的。

`IdleFaces` 现在是**预设式**的：每个预设是「参数子集 + 目标值」的组合，
例如「开心 = ParamBrowLY↑ + ParamEyeLSmile↑ + ParamMouthForm↑」。
预设只包含**模型实际拥有**的参数（加载时探测），不会设了不存在的参数
而让整张脸没变化。

状态机：

```
Idle（等待 8~20 s）→ FadeIn（0.25~0.5 s）→ Hold（2~5 s）→ FadeOut（0.25~0.5 s）→ Idle
```

输出是「参数名 → 权重」，由 `Live2DModel` **叠加**（而不是覆盖）刷进模型参数，
保证模型自己的待机动作与表情可以共存。

`TriggerWink()` 仍然可用：偶发单眼眨，权重随相位衰减。

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
| `IdleFaces` | ✅ 已接入 | 表情参数预设式随机小表情 + wink |
| `RegionSet` | ⬜ 已实现未接入 | 多边形热区（当前互动沿用 Cubism `HitAreas`） |

已接入部分的每帧流程（**顺序很重要**，见下方说明）：

```
每帧：
  cubism::ResetParametersToDefault()   // 复位到 moc3 默认值
  ApplyEyeBlink(dt)                    // 眨眼
  ApplyBreathing(dt)                   // 呼吸
  UpdateMotion(dt)                     // 动作（motion3.json）
  ApplyTracking(ctx, dt)               // 视线追踪（必须在动作之后）
  ApplyIdleFaces(dt)                   // 待机随机小表情
  ApplyPhysics(dt)                     // 物理（必须在视线之后）
  ApplyMouth(dt)                       // 口型
  cubism::UpdateModel()
```

**为什么视线追踪要放在动作之后**：几乎所有 Live2D 模型的待机动作都会动画
`ParamAngleX` / `ParamAngleY`（模型自己在摇头晃脑）。视线追踪放在动作之前
会被动作完全覆盖，「模型一直看鼠标」就完全看不到。放在之后并采用**叠加**
方式，即可做到「模型照常做自己的动作，同时眼睛跟着鼠标看」。

**为什么物理要放在视线之后**：物理的输入通常是头部/身体角度参数
（`ParamAngleX` / `ParamBodyAngleX`），必须等视线追踪把角度写进去，
物理才能算出「头转到哪，头发就甩到哪」。

**眼球的量纲**：头部角度参数（`ParamAngleX`）单位是「度」（典型范围 -30..30），
而眼球参数（`ParamEyeBallX`）是**归一化方向**（典型范围 -1..1）。
两者必须分别处理：眼球按参数自身范围换算，头部按角度换算。
早期实现对两者用同一个系数，导致眼球被写到 ±30 再钳到 ±1——
眼睛看起来「一直斜着看一个角落」而不是跟着鼠标转。

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

## 八、Live2D 物理（头发摇曳）

`api/live2d/Live2DPhysics.{hpp,cpp}` 实现 **Cubism 官方物理算法**（Verlet 摆锤链）。

算法与官方 `CubismPhysics` / `CubismPhysicsMath` 对齐：

1. 输入归一化（按 `Normalization.Position` / `Normalization.Angle` 缩放）；
2. 重力绕根部旋转（`RotationDirection` × `gravityDirection`）；
3. 空气阻力（`AIR_RESISTANCE = 5.0`）；
4. 距离约束（`Radius` 把摆锤拉回应有长度）；
5. 输出按 `Type`（X / Y / Angle）× `Scale` × `Reflect` 计算，再按 `Weight` 混合。

几个容易踩的官方细节：

- `delay = particle.delay * dt * 30.0`（30 fps 基准）；
- `normalize` 未反向时取负；
- 官方那个「平移旋转 y 用已旋转的 x」的怪癖。

**固定步长**：`physics3.json` 的 `Meta.Fps` 声明后按 `1/Fps` 固定步长推进，
帧间做输入插值、两步之间做输出插值（与 Cubism 5 一致）。

**写回策略**：官方 Cubism 是**覆盖**输出参数，那会抹掉模型自带的头发动画。
本项目改为**叠加**（物理值 + 当前值），使「动作姿势」与「物理惯性」共存。

物理是「叠加」而不是「覆盖」，还有一个副作用要注意：**物理写回的参数
会和动作写回的值叠加**，所以物理强度不能太大，否则动作幅度会被顶到参数上限。

### 物理文件的格式兼容

`physics3.json` 的 `PhysicsSettings` 在官方规范里是**数组**，
每个元素含 `Input[]` / `Output[]` / `Vertices[]`。
解析器用 `AsArray()` 兼容「单对象」写法，因此两种都能读。

---

## 九、VRM-VPet 风格骨骼动作（PMX / VRM）

`src/tools/model/BoneMotion.{hpp,cpp}` 实现参考实现的**骨骼动作**：

| 能力 | 说明 |
| --- | --- |
| 动作库 | `Nod()`（点头）、`Shake()`（摇头）、`Wave()`（挥手） |
| 播放器 | 播放 / 停止 / 淡入淡出 / 循环 / 随机待机触发 |
| 关键帧 | 骨骼名 + 时间 + 欧拉角（度）+ 权重 |
| 插值 | **按骨骼分组归并**后线性插值 |

「按骨骼分组归并」是关键：同一个骨骼有多条关键帧时，若不归并就会输出
多条互相覆盖的条目，插值结果完全错乱。

PMX 与 VRM 共用这套骨骼角色名（`head` / `spine` / `chest` / `leftArm` /
`rightArm` / `leftForearm` / `rightForearm`），由各后端映射到自己的骨骼索引。

PMX 的施加时机有个坑：它的 `ApplyBoneTransforms()` 开头会**复位骨骼**，
所以骨骼动作必须在该函数**末尾**施加；VRM 在 `UpdateIdle` 前已复位，可直接施加。

---

## 十、动作分组识别（待机为什么「完全不动」）

这是本项目踩过的最隐蔽的坑，值得单独记录。

### 现象

某些 Live2D 模型加载后**待机时一个动作都不播**，模型看上去完全静止，
但没有任何报错。

### 根因

这些模型（国内常见的一体成型模型包）把**全部动作**放在 `model3.json` 的
**空名组** `""` 下：

```json
"Motions": {
  "": [
    { "File": "motions/idle.motion3.json" },
    { "File": "motions/touch_head.motion3.json" },
    { "File": "motions/touch_body.motion3.json" }
  ]
}
```

组名是空串，不含任何语义。按组名分类会得到「**没有任何 Idle 组**」，
于是待机组找不到候选，动作调度器什么都不播。

### 修复

`ClassifyMotionFile()` 按**文件名**回退推断用途：

| 文件名关键字 | 推断结果 |
| --- | --- |
| `idle` / `stand` / `wait` / `breathe` | Idle（待机随机播） |
| `touch` / `tap` | Tap（并抽出命中区 `head` / `body`） |
| `pinch` / `shake` / `flick` | 对应互动类型 |
| `exp` / `emotion` / `face` | Expression（随机表情） |

互动类型**优先于** idle（`tap_idle` 应当算互动而不是待机）。

分组时按「用途 + 命中区」再拆成子组：所有 `touch_*` 归为互动组，
但 `touch_head` 与 `touch_body` 分开，这样点击头部能命中 `touch_head`、
点击身体能命中 `touch_body`。

### 另一个坑：`Meta.Loop`

待机动画几乎总是循环的（`"Meta": { "Duration": 6.0, "Loop": true }`）。
忽略这个字段会让待机动画播一轮就停 3~7 秒，桌宠「动一下、停很久」。
现在 `Meta.Loop` 会被解析并传给 `MotionDirector`，循环动作自己转下去。

### 还有一个坑：`Segments` 的贝塞尔段

官方规范（`Live2D/CubismSpecs/FileFormats/motion3.json.md`）原文：

> A segment identifier is followed by *1 point* in case of linear, stepped, and
> inverse stepped segments, that represents the end of the segment, or
> *3 points* in case of bézier segments, that represent P1, P2, P3.

即贝塞尔段的段标识后跟 **3 个点**（P1、P2、P3），其中 P3 是终点；
P1.t / P2.t 按 `1/3`、`2/3` 的比例固定。

官方 Haru 模型的实际数据：

```json
"Segments": [ 0, 0, 1, 0.167, 0, 0.333, 0, 0.5, 0, 1, ... ]
//  = 点(0s,0) + 段标识1 + P1(0.167s,0) P2(0.333s,0) P3(0.5s,0)
```

早期实现按「4 个数（x1,y1,x2,y2）」解析，会把控制点数量数错，
导致整条曲线的时间轴错位（动画节奏乱掉甚至「看起来不动」）。

---

## 十一、渲染「偏白」的根因与修复

### 现象

模型加载后整幅画面被一片白/黄色的光罩住，角色几乎看不见。

### 根因

`u_baseColor` 只设置了 alpha、**没有缩放 RGB**：

```cpp
// 错误
glUniform4f(baseColor, 1.0f, 1.0f, 1.0f, drawable.opacity);
```

官方 Cubism 的 `GetModelColorWithOpacity` 在 premultiplied 模式下会把
**RGB 也乘上不透明度**：

```cpp
modelColorRGBA.A *= opacity;
modelColorRGBA.R *= modelColorRGBA.A;   // G / B 同理
```

只设 A 不缩放 RGB 的后果是：**部件越透明，颜色却越亮**。
加法混合（发光）部件尤其明显——一点微光就把整块画面刷白。

实测（用户模型 nengdai_2，600×600，修复前后用同一套脚本、同一次渲染管线测量）：

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 白像素（三通道均 > 245） | 7.66%（27580 px） | **0.01%**（45 px） |
| 过曝像素（RGB 和 > 720） | 13.27%（47771 px） | **0.27%**（959 px） |

修复后的渲染结果见 [`live2d-render-fixed.png`](live2d-render-fixed.png)：
红色鸟居、灯笼辉光、角色五官、草木与水面全部清晰可辨。

### 修复

```cpp
const float op = drawable.opacity;
glUniform4f(baseColor, op, op, op, op);
```

### 顺带修好的三处

| 问题 | 修复 | 权威依据 |
| --- | --- | --- |
| 二次预乘 | 纹理上传前预乘 alpha，着色器不再乘第二次 | `FragShaderSrcPremultipliedAlpha.frag` |
| 加法混合 Alpha 累加 | `glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE)` | `CubismShader_OpenGLES2.cpp` 的 Add 分支 |
| 遮罩 API 命名 | `csmIsInverseMask` → `csmIsInvertedMask` | Core 4.0+ 头文件 |

---

<a id="native-render"></a>

## 十二、纯 C++ Live2D 渲染设计（参考项目）

### 参考了什么

用户指定的 `live2dwallpaper` 类项目，以及官方 Native 系列：

| 项目 | 关键点 |
| --- | --- |
| [Noyad/Live2dWallPaper](https://github.com/Noyad/Live2dWallPaper) | 纯 C++ 的 Live2D 桌面壁纸，直接链接 Cubism Core + 原生窗口，无浏览器内核 |
| [sawaYch/Live2D-Linux-Native-Sample](https://github.com/sawaYch/Live2D-Linux-Native-Sample) | Linux 下 Cubism Core + OpenGL 的最小可跑样例 |
| [Live2D/CubismAndroidLiveWallpaper](https://github.com/Live2D/CubismAndroidLiveWallpaper) | 官方动态壁纸：C++ 层更新参数、遍历 Drawable、原生绘制 |
| [Live2D/CubismNativeFramework](https://github.com/Live2D/CubismNativeFramework) / [CubismNativeSamples](https://github.com/Live2D/CubismNativeSamples) | 官方 Framework：`MotionQueueManager` / `CubismMotion` / `CubismRenderer_OpenGLES2` 的权威实现 |

**共同结论**：这些项目都**不依赖 Chromium / Electron / WebView**。C++ 直接链接 Cubism Core，
自己创建 GL（或 D3D / Vulkan / Metal）上下文，每帧的固定流程是
「写参数 → `csmUpdateModel` / `csmResetDrawableAllFlags` → 按 `renderOrder` 遍历 Drawable →
设置混合与遮罩 → 提交顶点/纹理 → 绘制网格」。原 Electron 版 PPet 是把 Cubism **Web** SDK 包在
浏览器里；本 C++ 版本来就没有浏览器，但重构前「播哪个动作、怎么淡入淡出、用什么混合」
这些决策仍内联在 GL 代码里，缺少官方 Framework 的结构。本次按上述设计重写这两层。

### 动作层：官方队列 + 交叉淡入淡出

官方 `CubismMotionQueueManager::StartMotion()` 在把新动作入队前，会把队列里**所有未结束**的
动作用各自的 `FadeOutTime` 标记为淡出；`DoUpdateMotion()` 按入队顺序逐个
`UpdateParameters()`；`CubismMotion::DoUpdateParameters()` 的参数权重为：

```text
tmpFadeIn  = fadeIn  <= 0 ? 1 : EasingSine((userTimeSeconds - fadeInStart) / fadeIn)
tmpFadeOut = fadeOut <= 0 ? 1 : EasingSine((endTime - userTimeSeconds) / fadeOut)
weight     = tmpFadeIn * tmpFadeOut
v          = sourceValue + (value - sourceValue) * weight
```

其中 `CubismMath::EasingSine(x) = 0.5 - 0.5 * cos(pi * x)`，`x` 钳到 `[0,1]`。

落地为纯逻辑模块 `api/live2d/MotionQueue.hpp`（零 GL / Cubism / Qt 依赖）：

- `StartMotion()` 把旧动作标记淡出，`Tick()` 推进队列并处理 `Meta.Loop` 循环；
- `Blends()` 按入队顺序输出**本帧所有生效动作**及其 easing 权重（含正在淡出的旧动作）；
- `FinishPrimary()` / `StopAll()` 供控制台 `action stop` 与 `re model` 使用。

`MotionDirector` 保留原有 `Configure()` / `Tick()` / 优先级 API，内部改用 `queue_`；
`Live2DModel::UpdateMotion()` 遍历 `Blends()`，按上面的官方公式把每条动作的参数**叠加**写进
Cubism 参数。修复前只维护一个 `current_` clip，动作切换是瞬间跳变；现在是交叉淡入淡出。

### 渲染层：决策-执行分离

官方 `CubismRenderer_OpenGLES2` 把「画哪些部件、按什么顺序、用什么混合、baseColor 是多少」
与「怎么提交 GL 命令」混在一起。本项目抽出 `api/live2d/CubismRenderPlan.hpp`：
输入 `DrawableDesc`（renderOrder / 可见性 / 混合模式 / 遮罩 / 纹理索引），
`BuildRenderPlan()` 输出 `std::vector<RenderOp>`；`Live2DModel::Render()` 只负责执行
`set glBlendFuncSeparate` + `uniform u_baseColor` + 提交几何，`DrawDrawable()` 只管几何与纹理。

官方三套混合（premultiplied alpha）：

| 模式 | RGB `src / dst` | Alpha `src / dst` |
| --- | --- | --- |
| Normal | `ONE` / `ONE_MINUS_SRC_ALPHA` | `ONE` / `ONE_MINUS_SRC_ALPHA` |
| Additive | `ONE` / `ONE` | `ZERO` / `ONE` |
| Multiplicative | `DST_COLOR` / `ONE_MINUS_SRC_ALPHA` | `ZERO` / `ONE` |

`ModelColorWithOpacity()` 对齐官方 `GetModelColorWithOpacity`：premultiplied 下
**RGB 与 A 都要乘 `opacity`**（只乘 A 会导致半透明部件偏白，见第十一章）。
遮罩绘制同样走 RenderOp，mask 的 baseColor 用 premultiplied 白色。

这样拆的好处：决策链可以脱离 GL 做单元测试；换 Vulkan / Metal 后端只需替换执行层；
渲染计划与官方 Framework 一一对应，便于逐条核对而不是靠肉眼看画面猜。

### 顺带修好的「表情切换无效」

四状态截图验证暴露出：模型只有表情参数、没有表情动作组时，
`SetExpressionPreset("happy")` 直接返回 false，且手动触发的 `IdleFaces` 状态机在
「待机随机表情」开关关闭时不会推进相位，包络恒为 0。修复：

- `SetExpressionPreset()` 在动作组播放失败时回退到 `IdleFaces::TriggerId(id)`；
- `ExpressionPresetIds()` 合并「表情动作组 + 参数预设」，控制台 `expression list` 不再为空；
- `IdleFaces` 增加 `manual_` 标记，手动触发表情独立于开关完整播放（淡入→停留→淡出→回中性）；
- `IdleFaces::Reset()` 在每次 `Load()` 时清空运行态，避免上一个状态的表情渗进下一张截图。

修复后 `nengdai_2` 的 `expression` 快照能看到
`ParamEyeLSmile / ParamEyeRSmile / ParamBrowLY / ParamBrowRY: 0 → 1.000`，
参数累计变化从 0.163 提升到 4.163，差异高亮图里红色像素集中在眼眉区域。

### 验证

| 证据 | 结果 |
| --- | --- |
| `motion_queue_unit` | 32 项检查（fade / 交叉淡入淡出 / loop / Finish / Stop / 优先级），失败 0 |
| `render_plan_unit` | 20 项检查（混合表 / premultiplied 颜色 / 稳定排序 / 无效部件过滤），失败 0 |
| `motion_director_unit` | 队列化后回归 72 项检查，失败 0 |
| 真实 SDK + `nengdai_2` 四状态截图 | 表情 / 口型 / 动作全部 PASS，见 [feature 验证报告](verify/feature/live2d_report.json) |

---

## 十三、相关源码

| 文件 | 职责 |
| --- | --- |
| `api/live2d/Stage.hpp` | 分层参数栈 / 姿态总线 / 三通道视线 / IdleDirector / IdleFaces / RegionSet / MouthDirector |
| `api/live2d/Live2DModel.cpp` | 把上面的输出刷进 Cubism 参数（`ParamAngleX` / `ParamEyeOpenX` / `ParamMouthOpenY` …） |
| `api/live2d/MotionDirector.hpp` | 动作组调度（待机 / 互动 / 表情），与 Stage 互补 |
| `api/live2d/Live2DPhysics.{hpp,cpp}` | Cubism 官方物理（头发摇曳） |
| `api/live2d/Live2DMotion.{hpp,cpp}` | motion3.json 解析（`Segments` 与旧版 `Curve` 两种写法） |
| `api/live2d/Live2DManifest.{hpp,cpp}` | model3.json 解析（含动作文件名、物理文件路径） |
| `src/tools/model/BoneMotion.{hpp,cpp}` | VRM-VPet 风格骨骼动作（PMX / VRM 共用） |
| `src/tools/model/IdleAnimator.hpp` | PMX / VRM 程序化待机（呼吸 / 重心 / 头部 / 手臂 / 腿部） |
| `api/live2d/MotionQueue.hpp` | 官方 `MotionQueueManager` / `CubismMotion` 风格动作队列（交叉淡入淡出，纯逻辑） |
| `api/live2d/CubismRenderPlan.hpp` | 渲染计划：Drawable → `RenderOp` 决策-执行分离（混合 / 遮罩 / premultiplied 颜色，纯逻辑） |
| `src/tools/app/ConsoleCommand.hpp` | 控制台指令分词、解析、表格与帮助（纯逻辑） |
| `src/tools/app/Console.{hpp,cpp}` | 后台 stdin 读取 + 主线程命令队列 |
| `src/tools/app/AppConsole.cpp` | 控制台命令执行器（模型 / 调试 / 重载 / 动作 / 表情 / 气泡） |
| `tools/feature_verify.cpp` | 四状态截图与像素 + 参数差异判定 |
| `tools/gen_feature_compare.py` | 2x2 对比图、局部放大、差异高亮、全局差异掩膜 |

单元测试：

| 测试 | 覆盖 |
| --- | --- |
| `tools/live2d_stage_test.cpp` | Stage 各组件 + 手动触发表情不受开关影响（51 项检查） |
| `tools/live2d_physics_test.cpp` | 物理解析 + 摆锤求解 + 压力测试 |
| `tools/live2d_motion_group_test.cpp` | 动作分组 / `Meta.Loop` / `Segments` 解析 |
| `tools/live2d_manifest_test.cpp` | model3.json 解析 |
| `tools/bone_motion_test.cpp` | 骨骼动作库与播放器 |
| `tools/motion_director_test.cpp` | 动作调度器（队列化后 72 项检查） |
| `tools/motion_queue_test.cpp` | 官方动作队列 fade / 交叉淡入淡出 / loop / Stop（32 项检查） |
| `tools/render_plan_test.cpp` | 混合表 / premultiplied 颜色 / 稳定排序 / 无效部件过滤（20 项检查） |
| `tools/console_command_test.cpp` | 指令分词 / 引号 / 表格 / 帮助（28 项检查） |

相关文档：[表情管理与语言功能](expressions.md) · [使用与配置 · 交互式控制台](usage.md#交互式控制台) ·
[项目结构](architecture.md) · [四状态截图验证报告](verify/feature/)
