# 表情管理与语言功能

本文说明 PPet 的两套「角色表现」能力：

1. **表情管理**（PMX / VRM / Live2D）——参考 [Tanorika/VRM-VPet](https://github.com/Tanorika/VRM-VPet) 的
   `ExpressionManager` / `ExpressionTimeline` / `MotionManager` 组织方式；
2. **语言功能**（台词 / 气泡 / 口型）——参考 [FULASKA/-live2d-](https://github.com/FULASKA/-live2d-) 的
   `phrases` / `bubble` / `mouth` 三个模块，三种模型格式共用。

两套能力都做成**纯头文件、零 GL / 零 Qt / 零文件 IO** 的逻辑层，可以脱离渲染上下文做单元测试
（`ppet_expression_director_test` / `ppet_dialogue_system_test` / `ppet_live2d_stage_test`）。

---

## 快速上手

| 想做的事 | 怎么做 |
| --- | --- |
| 切换到某个表情 | 托盘菜单 → **表情** → 选择预设（自然 / 开心 / 放松 / 生气 / 难过 / 惊讶） |
| 让桌宠说一句话 | 托盘菜单 → **说一句**（随机抽一条），或直接**点击桌宠**触发问候 |
| 开关台词气泡 | 托盘菜单 → **台词气泡**（关掉后桌宠照常动嘴，只是不显示文字） |
| 看当前表情 / 台词 / 口型 | 托盘菜单 → **模型诊断** → **表情与说话** 页签 |

---

## 一、表情管理（PMX / VRM）

### 1.1 核心概念：通道与预设解耦

模型里的表情是**一条条通道**（PMX 的 morph、VRM 的 blendshape），而用户想的是
「开心」「生气」这种**语义预设**。两者的关系由 `ExpressionDirector` 负责：

```
模型真实通道   ←── Bind() ──   表情通道（morph 名 / expression 名）
                                  ↓
                             预设（逻辑名 → 权重）
                                  ↓
                        过渡 / 眨眼 / 动作叠层 / 二元量化
                                  ↓
                             最终权重 → morph 顶点变形
```

后端只做两件事：把通道名喂给 `Bind()`，把 `Weights()` 写进 morph。

### 1.2 内置预设

| id | 名称 | 逻辑通道 | 过渡时长 |
| --- | --- | --- | --- |
| `neutral` | 自然 | （全部归零） | 0.30 s |
| `happy` | 开心 | `happy` | 0.30 s |
| `relaxed` | 放松 | `relaxed` | 0.35 s |
| `angry` | 生气 | `angry` | 0.25 s |
| `sad` | 难过 | `sad` | 0.35 s |
| `surprised` | 惊讶 | `surprised` | 0.20 s |

预设**可用性是运行时判定的**：只有当预设需要的每一个逻辑通道都能在模型里解析到，
该预设才可用。解析不到就标记为不可用，**绝不写越界下标**。

### 1.3 别名解析

不同模型的命名习惯差异很大，同一个「开心」在不同文件里可能是
`happy` / `joy` / `fun` / `smile` / `笑` / `笑い` / `开心`。别名表把逻辑名映射到候选：

| 逻辑名 | 候选（不区分大小写，`\|` 分隔） |
| --- | --- |
| `happy` | `happy` `joy` `fun` `smile` `笑` `笑い` `开心` `喜悦` |
| `sad` | `sad` `sorrow` `cry` `悲` `泣` `难过` `悲伤` |
| `relaxed` | `relaxed` `fun` `calm` `安` `放松` `安心` |
| `angry` | `angry` `anger` `mad` `怒` `生气` `愤怒` |
| `surprised` | `surprised` `surprise` `shock` `惊` `惊讶` `びっくり` |
| `blink` | `blink` `blinkboth` `眨眼` `瞬き` `まばたき` `闭眼` |
| `blinkLeft` | `blinkleft` `blink_l` `blinkl` `winkleft` `眨左` `左眨眼` `ウィンク左` |
| `blinkRight` | `blinkright` `blink_r` `blinkr` `winkright` `眨右` `右眨眼` `ウィンク右` |
| `aa` `ih` `ou` `ee` `oh` | 拉丁拼音 + **日文假名**（`あ` `い` `う` `え` `お`）+ `口型a` / `mouth_a` / `vowel_a` 等 |

因此 PMX 的日文口型 morph（`あ`/`い`/`う`/`え`/`お`）与 VRM 1.0 的拼音口型（`aa`/`ih`/…）
会被同一套逻辑通道命中。

### 1.4 过渡与叠加层

| 机制 | 行为 | 对齐 VRM-VPet 的 |
| --- | --- | --- |
| **平滑过渡** | 切换预设时用 `smoothstep` 插值，不是瞬间跳变 | `ExpressionManager.setExpression` |
| **自动眨眼** | 独立叠层，用 `max` 叠加，**不写入**用户当前选的表情 | `setBlink` |
| **动作表情** | 动作播放期间的临时表情层，结束后 **0.2 s 淡出**回落到用户表情 | `setMotionWeights` |
| **二元表情** | VRM 1.0 的 `isBinary`（如 `blink`）量化成 0 / 1，半睁半闭很假 | — |

### 1.5 表情时间轴

`ExpressionTimeline` 支持表情关键帧的录制 / 覆盖 / 删除 / 采样 / 导出：

```cpp
ExpressionTimeline timeline;
timeline.Record(0.0f, 2.0f, {{"happy", 0.0f}});
timeline.Record(0.5f, 2.0f, {{"happy", 1.0f}});
timeline.Record(1.0f, 2.0f, {{"happy", 0.2f}, {"blinkLeft", 1.0f}});

const auto frames = timeline.Sample(0.75f, ClipEasing::EaseInOut);
```

采样时取前后两个关键帧做插值，缺失的一侧按 0 处理，
`EaseInOut` 缓动走 `smoothstep`。

### 1.6 PMX / VRM 的口型叠加层

口型（说话时的元音 morph）是**叠加在当前表情之上**的一层，不是替换：

```
最终 morph 权重 = max(表情调度器算出的权重, 口型叠加层)
```

用 `max` 而不是相加，是因为同一通道上两个来源叠加可能超过 1，
造成顶点形变过度（模型嘴部「炸开」）。

> 这也是为什么 `IModel::SetExpressionWeights` 在 PMX / VRM 后端里写的是**口型层**
> 而不是 `ExpressionDirector::PreviewWeights` —— 后者会清空全部基础权重，
> 导致每帧写口型都把用户选的「开心 / 生气」表情抹掉。

### 1.7 Live2D 的表情

Live2D 的「表情」通常是**动作组**（如 `ExpHappy`），由 `MotionDirector` 以
`MotionPriority::Expression` 优先级播放。托盘的「表情」子菜单会列出模型里
所有 `MotionGroupKind::Expression` 类型的动作组。

---

## 二、语言功能（台词 / 气泡 / 口型）

三种模型格式共用同一套语言层，位置在 `src/tools/agent/DialogueSystem.hpp`。

### 2.1 台词库（Phrases）

台词按**七种状态**分档，每档至少 3 句，避免很快复读：

| 状态 | 枚举 | 用途 |
| --- | --- | --- |
| `idle` | `AgentStateKind::Idle` | 待机碎碎念 |
| `thinking` | `Thinking` | 思考中 |
| `working` | `Working` | 执行任务中 |
| `waiting` | `Waiting` | 等待输入 / 工具返回 |
| `success` | `Success` | 成功 |
| `error` | `Error` | 出错 |
| `greeting` | `Greeting` | 打招呼（被摸头 / 点击） |

抽取规则：按状态随机抽，**不连续抽中同一句**（`Pick(kind, avoidText)`）；
该档为空时回退到 `idle` 档。

### 2.2 气泡状态机（bubble）

`SpeechController` 控制「现在该不该说话、说什么、说多久」：

| 规则 | 默认值 | 说明 |
| --- | --- | --- |
| 优先级 | 互动 2 / 状态 1 / 待机 0 | 更高优先级可以打断正在说的话 |
| 单句时长 | 3.5 s + 0.06 s/字，上限 8 s | 与气泡的字数上限配套 |
| 字数上限 | 24 个**字符** | 按 UTF-8 **码点**截断（不会把汉字劈成乱码），超出补 `…` |
| 说完冷却 | 0.8 s | 说完到能接下一句的最短间隔 |
| 待机间隔 | 12 ~ 25 s 随机 | 待机碎碎念的最小/最大间隔 |

### 2.3 口型（Mouth）

`LipSync` 把「说话进度 + 当前情绪」转换成口型：

- **开口度** `MouthOpen() ∈ [0,1]`：用**音节包络**驱动（`sin` 半波），
  频率随说话强度提高（说得越快音节越密），而不是匀速张嘴；
- **元音嘴形** `GetVowels()`：每个音节带一个元音（`aa`/`ih`/`ou`/`ee`/`oh`），
  开口时该元音权重拉满、其余回落；
- **情绪嘴形** `MouthForm() ∈ [-1,1]`：成功 / 问候时嘴角上扬（+0.4），
  出错时嘴角下压（−0.4）；
- **呼吸微动**：静止时嘴也有轻微起伏（幅度 0.06）。

三种后端的落地方式：

| 后端 | 接口 | 落地 |
| --- | --- | --- |
| Live2D | `IModel::SetLipSync(open, form)` | `ParamMouthOpenY` / `ParamMouthForm`，每帧平滑追向目标 |
| PMX | `IModel::SetExpressionWeights` | 元音 morph 叠加层（`あ`/`い`/`う`/`え`/`お`） |
| VRM | `IModel::SetExpressionWeights` | 元音 blendshape 叠加层（`aa`/`ih`/`ou`/`ee`/`oh`） |

### 2.4 气泡窗口

台词气泡是**独立的 Qt 窗口**（`src/ui/BubbleWidget.hpp`），无边框、跟随桌宠窗口上方居中。

遵循项目既有约定：**置顶只作用于渲染窗口（桌宠本体）**，Qt GUI（气泡 / 设置 / 诊断）
保持普通窗口层级，不抢用户正在操作的其他窗口的焦点。

### 2.5 AI 对话接口

`AgentStateKind` 枚举就是给外部 Agent 用的**状态接口**：外部 Agent 推送状态后，
调用 `SpeechController::SayState(kind, priority)` 即可让桌宠说对应状态的台词。
当前默认实现是「本地随机台词」，接入后由外部接管「该说什么」。

---

## 三、诊断

托盘 → **模型诊断** → **表情与说话** 页签实时显示：

- **表情状态**：例如「表情预设：开心 / 动作表情叠层 30%」
- **表情摘要**：例如「预设 6 / 通道 12 / 当前 开心」
- **当前台词**：`「今天也要加油呀～」 60%`
- **口型**：`开口 0.42 / 嘴形 -0.25`

页签内容**每次刷新都重绘**（台词与口型逐帧变化），
而骨架 / 贴图 / 材质等列表只在模型变化时重建，避免每 200 ms 闪烁。

---

## 四、相关源码

| 文件 | 职责 |
| --- | --- |
| `src/tools/model/ExpressionDirector.hpp` | 表情调度：预设 / 别名 / 过渡 / 眨眼 / 动作叠层 / 时间轴（纯逻辑） |
| `src/tools/agent/DialogueSystem.hpp` | 台词库 / 气泡状态机 / 口型同步（纯逻辑） |
| `src/ui/BubbleWidget.hpp/.cpp` | 台词气泡窗口（Qt） |
| `src/tools/model/pmx/PmxModel.cpp` | PMX：绑定 morph 通道、应用口型叠加层、驱动顶点变形 |
| `src/tools/model/vrm/VrmModel.cpp` | VRM：绑定 expression 通道、应用口型叠加层、驱动 morph |
| `api/live2d/Live2DModel.cpp` | Live2D：表情动作组、口型参数刷入 |
| `api/live2d/Stage.hpp` | Live2D 分层运行时（视线 / 待机 / 小表情 / 热区 / 口型） |

单元测试：`tools/expression_director_test.cpp`、`tools/dialogue_system_test.cpp`、
`tools/live2d_stage_test.cpp`；UI 冒烟：`tools/debug_panel_smoke.cpp`、
`tools/bubble_widget_smoke.cpp`。

相关文档：[使用与配置](usage.md) · [Live2D 分层运行时](live2d-stage.md) ·
[调试与日志](debugging.md) · [项目结构](architecture.md)
