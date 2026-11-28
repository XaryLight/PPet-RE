# VRM 渲染与弹簧骨物理修复（以千璃0711.vrm 为验证样本）

← [返回 README](README.md)

---

## 背景

用户反馈千璃0711.vrm（UniVRM 0.98 导出的 **VRM 0.x** 模型，27 万顶点）有两个问题：

1. **模型渲染有问题**；
2. **没有自然物理引擎效果**。

用该真实模型逐层排查后，定位到两个子系统里的多个确定性 bug，全部修复并用该模型实测验证。

---

## 一、渲染问题

### 1. VRM 0.x 的 MToon 材质参数完全没解析

VRM 0.x 把 MToon 材质参数放在根级 `extensions.VRM.materialProperties[]`（按材质名匹配），
而解析器只认 VRM 1.0 的 `VRMC_materials_mtoon`。结果该模型 **28 个材质的描边色、阴影色、
边缘光、shadeShift/shadeToony 全部丢失**，渲染成扁平的默认卡通色。

修复：`GltfParser.cpp` 新增 `ApplyVrm0MaterialProperties()`，按材质名匹配并映射
`floatProperties` / `vectorProperties` / `textureProperties` 到 `GltfMaterial`。

### 2. alphaMode / alphaCutoff 解析后被丢弃

glTF 材质带 `alphaMode: MASK/BLEND`，但 `Material` 结构没有对应字段，转换时丢失；
渲染器改用 `diffuse.w` 猜深度写入，片元着色器也没有 alphaTest。后果：

- 头发/睫毛/裙边的**镂空贴图渲染成实心块**；
- 半透明眼睛/高光错误写深度，遮挡关系全乱。

修复：`Material` 增加 `alphaMode`（Opaque/Mask/Blend）+ `alphaCutoff`；
着色器加 `uAlphaTest`/`uAlphaCutoff` 并在 MASK 下 `discard`；
渲染器按模式决定深度写入（BLEND 不写，MASK/OPAQUE 写）。

### 3. morph 预算过小导致表情/口型全部失效

高精度模型 27 万顶点，每个 morph 占 6.5 MiB，41 个就超过旧的 256 MiB 预算，
morph 映射表建不全，VRM 0.x 的 70 组 blendShape 表情**一个都绑定不上**。

修复：预算从 256 MiB 提到 1 GiB（仍是硬上限，防恶意文件 OOM）。
实测表情数从 **0 → 66**，morph 从 41 → 117。

---

## 二、弹簧骨物理问题

旧 `UpdateSpringBones` 有三处错误，叠加导致「完全没有自然物理效果」：

| 项 | 旧实现（错误） | VRM 规范（修复后） |
| --- | --- | --- |
| 惯性 | `(cur - prev) × dragForce` | `(cur - prev) × (1 - dragForce)` |
| 回正力 | **完全没有** | `dt × restDirection × stiffness` |
| 重力 | `gravityPower × dt²` | `gravityPower × dt` |
| 碰撞 | 每骨只取 1 个碰撞球 | 关联的全部碰撞球都做推出 |

- **缺 stiffness 回正力是主因**：只有单向重力时，头发/裙摆垂下后就静止不动，永不回摆。
- 惯性乘反了 dragForce，阻尼方向错误。
- 重力乘了 dt²，量级比规范小一个 dt，几乎看不出。
- 该模型头发链关联 10 个碰撞组，旧代码只用第一个球，头发会穿过身体。

积分公式抽成纯逻辑模块 `src/tools/model/SpringIntegrator.hpp`（零 GL/Qt 依赖，可单测），
含 NaN/Inf 防御（上游污染时退回有限值，避免模型「炸开」）。

---

## 三、验证数据（千璃0711.vrm 实测）

用 `ppet_qianli_probe` 加载真实模型、驱动 120 帧：

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 材质 | MASK 被当 opaque（实心块） | MASK=23 / BLEND=5 正确处理 |
| MToon 描边 | 0 | 20 |
| MToon 阴影贴图 | 0 | 28 |
| shadeShift/shadeToony/rimFresnel | 默认值 | 0.0 / 0.642 / 100.0（正确解析）|
| 表情 | 0 | 66 |
| morph | 41（预算截断）| 117 |
| 弹簧骨 | 54 根但无回正力 | 54 根，stiffness 0.277~1.0，回正/阻尼/重力生效 |

测试：stub 68/68、nocubism 67/67 全绿；`spring_integrator_test` 8 断言覆盖静止稳定、
回正力、惯性方向、重力 dt 量级、骨长约束、NaN 防御。

> 注：千璃模型仅用于只读验证，不入库。`ppet_qianli_probe` 是手动工具（需传 `.vrm` 路径），
> 不加入 ctest 矩阵。
