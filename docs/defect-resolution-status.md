# 第二部分缺陷处置状态总表

> 对应 `PPet-RE-项目深度分析报告.md.txt` 第二部分（2.1 安全 / 2.2 正确性 / 2.3 架构 / 2.4 并发 / 2.5 数值渲染 / 2.6 工程）。
> 按「先堵安全、再补正确性、逐步偿还架构债」推进，每批完成即提交推送。
> 状态：**✅ 已修复** / **⚠️ 部分或保守版** / **⏸ 有意延后（本批已全部清空）**。

## 2.1 安全边界（P0/P1）——全部修复

| 编号 | 主题 | 状态 | 关键提交 / 位置 |
| --- | --- | --- | --- |
| 2.1.1 | ZIP 炸弹防护 | ✅ | `49d078d` ZipArchive 输出/压缩比/累计预算 |
| 2.1.2 | EOCD 整数溢出 | ✅ | `49d078d` uint64 提升 |
| 2.1.3 | glTF sparse accessor | ✅ | `49d078d` GltfParser ApplySparse |
| 2.1.4 | VRM 图片解码预算 | ✅ | `49d078d` VrmImage STBI_MAX_DIMENSIONS + try/catch |
| 2.1.5 | 配置类型错误/非法 UTF-8 崩溃 | ✅ | `49d078d` 安全 getter + UTF-8 净化 |
| 2.1.6 | 外部 URI 目录穿越 | ✅ | `332e11c` ResolveUriInsideBase |
| 2.1.7 | VrmModel 有符号溢出 | ✅ | `332e11c` int64 中间量 |
| 2.1.8 | ReadFileBytes 无上限/bad_alloc | ✅ | `332e11c` 2GB 上限 + 捕获 |
| 2.1.9 | Log 阈值锁外读取 | ✅ | `332e11c` Path() 返回副本 |

## 2.2 正确性（P1）——全部修复或明确取舍

| 编号 | 主题 | 状态 | 说明 |
| --- | --- | --- | --- |
| 2.2.1 | WS_EX_LAYERED 矛盾 | ✅ | `332e11c` 显式清除 |
| 2.2.2 | macOS 自启动查询恒 false | ✅ | `332e11c` plist ProgramArguments 解析 |
| 2.2.3 | macOS 虚拟桌面并集错误 | ✅ | `332e11c` 显式 firstScreen + y 翻转 |
| 2.2.4 | Live2D 物理双重叠加 | ✅ | `ffca404` 直接写回 Evaluate 混合值 |
| 2.2.5 | 循环动作周期性淡出 | ✅ | `ffca404` 仅非 loop 淡出 |
| 2.2.6 | Stage 参数栈未接入 | ✅ | `419a93c` 追踪层 L1 gaze / L3 idle 加性写入 ParameterStack，Commit 后统一回写；`live2d_stage_unit` 视觉等价断言 |
| 2.2.7 | stub Part API 不可测 | ✅ | `c388769` stub Core 补齐 Part API，并修复 4 个潜伏内存/解析 bug；`stub_part_api_unit` 覆盖 Count/Find/Set/Reset + drawable 传播 |
| 2.2.8 | 同深度父子骨骼断链 | ✅ | `ffca404` 深度+父节点索引排序 + 拓扑修正 |
| 2.2.9 | CUBICSPLINE morph 丢弃 | ✅ | `ffca404` components/3 |
| 2.2.10 | 插件禁用态重名替换 | ✅ | `ffca404` 新实例默认启用 + OnEnable |
| 2.2.11 | 优先级语义与文档不符 | ✅ | `ffca404` 注释对齐实现 |
| 2.2.12 | SkinnedMesh 缺 textures | ✅ | `ffca404` 新增 TextureRef + 两端填充 |
| 2.2.13 | 口型/表情数组互斥 | ✅ | `ffca404` 独立口型叠加层（PmxModel/VrmModel） |

## 2.3 架构（P2）——全部落地（2.3.1 为保守版）

| 编号 | 主题 | 状态 | 说明 |
| --- | --- | --- | --- |
| 2.3.1 | App God Object 拆分 | ⚠️ | `10d1e39` 保守落地：抽出 `ConsoleFormat` 纯函数模块（10 函数 + 约 105 行 debug 渲染）并加 70 项单测；App 仍是主控制器，成员级拆分留待后续 |
| 2.3.2 | 巨型 TU 拆分 | ✅ | `f7596d2` 抽出 header-only `ToonShaders.hpp`，Renderer.cpp 由 1151 行降至 957 行；保留 stb 多前缀避免符号冲突 |
| 2.3.3 | 插件动态加载 | ✅ | `8b54f15` dlopen/LoadLibrary + ABI 版本校验 + shared_ptr 自定义 deleter；`dynamic_plugin_unit` 覆盖加载/释放/目录扫描 |
| 2.3.4 | IModel 契约统一 | ✅ | `991b0a3` 能力位 SupportsExpressions/LipSync/Motion + 统一 SetMouth + 工厂注册表；`model_capability_unit` |
| 2.3.5 | GPU 资源建立无异常捕获 | ✅ | `a1b264b` gpuInitFailed_ + try/catch 降级 |
| 2.3.6 | CMake 自动登记脆弱 | ✅ | `3b16d5d` `ppet_register_i18n_consumer` + basename 匹配 + 零消费者 FATAL 断言 |
| 2.3.7 | 配置保存非原子 | ✅ | `1ca01d6` 同目录 .tmp + rename 原子写，失败清理后回退；**严格保留** A: 盘「直写→暂存→copy_file」流程（见 `docs/debugging.md`）；`config_unit` 断言无 .tmp 残留 |

## 2.4 并发与生命周期（P2/P3）

| 编号 | 主题 | 状态 | 说明 |
| --- | --- | --- | --- |
| 2.4.1 | detach 线程访问全局 iostream | ✅ | `76732b0` 读线程改用 OS 级 read/ReadFile 逐字节读，prompt 由主线程输出；彻底消除全局 iostream UB，不引入 join 死锁 |
| 2.4.2 | CrashHandler 非 async-signal-safe | ✅ | `a1b264b` 改 GetStdHandle+WriteFile，去 fputs/snprintf |
| 2.4.3 | 事件分发无锁 | ✅ | `a1b264b` recursive_mutex + DepthGuard |
| 2.4.4 | stdout 无锁交错 | ✅ | `a1b264b` OutputMutex + WriteRaw |
| 2.4.5 | 模态框/deleteLater 竞态 | ✅ | `21c0aea` 确认框改非模态 + weak_ptr<bool> alive 哨兵；Shutdown 先 reset 再 deleteLater |
| 2.4.6 | 双全局事件泵 | ✅ | `a1b264b` 移除每帧第二次 PumpEvents |
| 2.4.7 | Shutdown 双调用/裸指针 | ✅ | `a1b264b` bubble_ 改 unique_ptr（幂等保持） |

## 2.5 数值与渲染（P2/P3）——全部修复或保守版

| 编号 | 主题 | 状态 | 说明 |
| --- | --- | --- | --- |
| 2.5.1 | GL 状态泄漏 | ✅ | `75efc4f` GlStateGuard RAII 保存/恢复全部状态 |
| 2.5.2 | 无界 glGetError 循环 | ✅ | `2e7b691` 32 次上限 |
| 2.5.3 | PhaseOf 负相位 | ✅ | `2e7b691` 加 2π 修正 |
| 2.5.4 | 着色器编译失败每帧重试 | ✅ | `2e7b691` shaderCompileFailed_ 只试一次 |
| 2.5.5 | 矩阵/数值稳定性 | ✅ | `06e6a29` int64 钳制 + 稳健 Length/Normalize + TryInverse 相对阈值 + 投影除零校验 |
| 2.5.6 | 每帧反解相机 + 全量骨骼 + 超大 UBO | ✅ | `5b16ab0` UBO 按实际骨骼容量（16 对齐、封顶硬件上限）+ `boneMatrices_` 复用；容量变化时 reset shader 防越界 |
| 2.5.7 | 别名表悬垂 + 眨眼包络 | ✅ | `2e7b691` pool 先填后取 c_str + 哨兵 |
| 2.5.8 | 语义识别子串误匹配 | ✅ | `2e7b691` 词边界匹配 + 尾部编号剥离 |
| 2.5.9 | 待机热改频率跳变 | ✅ | `06e6a29` 相位改累积式 + 注释对齐 |
| 2.5.10 | morph 内存无预算 | ✅ | `75efc4f` kMaxMorphBytes + 饱和检查 |

## 2.6 工程与测试（P2/P3）

| 编号 | 主题 | 状态 | 说明 |
| --- | --- | --- | --- |
| 2.6.1 | fuzz 无效且未接入 CTest | ✅ | `2e7b691` 临时目录 + pmx/vrm_fuzz_unit 接入 CTest |
| 2.6.2 | i18n 生成器解析脆弱 | ✅ | `2e7b691` 控制字符转义 + `06e6a29` 伪成员剥离/重复键检测 |
| 2.6.3 | 测试盲区 | ✅ | `06e6a29` math_stability/idle 热改/plugin 嵌套与 Config/i18n 伪成员与重复键 |
| 2.6.4 | 一致性杂项 | ✅ | `2e7b691` FileIO 循环写/CRC 校验 + `fe851b2` totalBytes 饱和加法/64 位缓存哈希 |

## 第三 / 四部分补充落地（本轮）

| 报告项 | 主题 | 状态 | 关键提交 / 说明 |
| --- | --- | --- | --- |
| 3.4.11 | morph 法线归一化与权重钳制 | ✅ | `071b746` `ApplyMorphs` 权重钳到 [0,1]、忽略 NaN/inf，叠加后法线重新归一化；`morph_normal_unit` |
| 4.2.3 | RuntimeStats 导出 JSON/CSV | ✅ | `d61d9ea` 新增纯 C++ `RuntimeStatsExport`（含转义/往返）+ 诊断面板导出按钮 + 三键 i18n；`runtime_stats_export_unit` |
| 3.5.4 | CMake 测试注册样板 | ✅ | `fab8371` 抽出 `ppet_add_simple_test`，替换 17 个标准测试；带特殊配置的 fuzz/smoke 保持显式 |
| 4.2.1 | vcpkg 依赖管理 | ✅ | 根目录 `vcpkg.json`（manifest，真实 baseline）+ `CMakePresets.json` 的 `vcpkg-*` opt-in 变体；原有三配置完全不感知 vcpkg、离线可构建 |
| 动画过渡 | 播放间自然过渡（新增需求） | ✅ | `TransitionEasing.hpp` 统一曲线 + 权重守恒 crossfade，修复骨骼切换过冲；`transition_easing_unit` |

> 第四部分其余建议的落地情况：CMake Presets / CI / ADR / `--doctor` / 本地化与 `--lang` / 自适应帧率 / 日志轮转 / vcpkg 依赖管理（4.2.1）均已完成；AI 对话与 TTS（4.1.1）、全局快捷键真正注册（4.1.4）属高成本/跨 GUI 平台项，本轮未纳入。

## 统计

- 第二部分共 **48** 个缺陷条目，**已全部落地**（47 项已修复 + 1 项保守落地）：
  - 原 37 项在此批之前已修复；
  - 原延后 11 项已在隔离副本 `refactor/deferred-11` 中逐项重构验证，提交区间 `3b16d5d` → `10d1e39`；
  - 其中 2.3.1（App God Object）为 **⚠️ 保守落地**：剥离可单测的纯逻辑，App 成员级拆分未做。
- 2.3.7 特别说明：原子写仅作为第一路径，失败即回退，**严格保留**用户要求的 A: 盘「直写→暂存→拷贝」语义。
- 测试体系从 45 项增至 **65 项**（stub 配置）/ **64 项**（nocubism、cubism 配置），三配置全绿；ASAN 下 6 个 GL/Qt 图形用例因 Ubuntu 22.04 的 Mesa/Qt 退出期第三方静态缓存泄漏被 `ppet_no_leak_check()` 定向关闭泄漏检测（渲染本身全 PASS，越界/UBSAN 与纯逻辑用例的泄漏检测均保留），另两个既有 Qt GUI 对话框内存泄漏用例仍用 `ctest -E` 排除（均与改动无关）。
