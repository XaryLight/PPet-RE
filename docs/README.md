# 文档索引

← [返回 README](../README.md)

| 文档 | 适合什么时候看 |
| --- | --- |
| [architecture.md](architecture.md) | **目录结构、根索引约定、`src/ui` / `src/tools` / `api` 分工**、旧新路径速查 |
| [build.md](build.md) | 第一次编译、配置 Live2D SDK、Windows/CLion 工具链报错 |
| [windows-test.md](windows-test.md) | **Windows 11 + N 卡 + llvm-mingw + Qt6 + SDK 5.x 真机验收** |
| [models.md](models.md) | 导入模型、**PMX 白模**、`.zip` 该怎么打包、中文路径 |
| [usage.md](usage.md) | 日常使用、托盘菜单、**交互式控制台指令手册**、设置面板、**兼容模式 / 帧率上限命令行参数**、配置字段 |
| [expressions.md](expressions.md) | **表情管理（预设 / 别名 / 过渡 / 叠加层）、语言功能（台词 / 气泡 / 口型）、待机随机表情** |
| [live2d-stage.md](live2d-stage.md) | **Live2D 分层运行时：动作分组 / 待机循环 / 物理头发摇曳 / 视线追踪 / 随机小表情 / 纯 C++ 渲染设计（参考项目）** |
| [debugging.md](debugging.md) | 模型不显示、诊断面板、失败弹窗、`last.log` 日志格式（含**多行续行**）与级别、**`0xC0000005` 崩溃与第三方 overlay 注入、系统级卡顿排查** |
| [development.md](development.md) | 架构分层、C++23 使用点、测试清单、**三配置构建矩阵**、**MinGW 交叉编译验证 Windows 分支**、**四状态截图对比验证**、新增模型后端 |
| [../api/README.md](../api/README.md) | Live2D SDK 集成层、第三方库引用头 |
| [../Compiler/README.md](../Compiler/README.md) | 修改界面文案、文案翻译器用法 |

图片与验证证据：

- [settings-panel.png](settings-panel.png) —— 设置面板截图
- [live2d-verify.png](live2d-verify.png) —— Live2D 渲染验证截图
- [live2d-render-fixed.png](live2d-render-fixed.png) —— **修复「渲染偏白」后的真实模型渲染结果**（白像素 7.66% → 0.01%）
- [verify/](verify/) —— **VRM / PMX(zip) / moc3 三类模型的渲染截图与 JSON 报告**（Linux 离屏等价验证）
- [console_demo.txt](verify/console_demo.txt) —— **交互式控制台完整实跑日志**（`help` / `list` / `add` / `select` / `debug status|res|expression` / `expression list|happy` / `action list` / `speak` / `status` / `quit`，可见 `renderingOk: true`、`console: interactive`）
- [console_features.txt](verify/console_features.txt) —— **本轮 10 项增强的实跑证据**：`list` 显示中文名与 `*` 当前项、`rename` 改名、`speak` 长台词自动换行、`status` 里的 `cmdDropped: 0` / `autoFitWin: on` / `window: 648x600` / `bubbleMaxW: 720 px` / `osCursor: (640,512)（与点击穿透无关）`，全程 `[CONSOLE]` 日志数为 0
- [verify/feature/](verify/feature/) —— **PMX 与 Live2D 各一个模型的「中性 / 表情 / 口型 / 动作」四状态截图、JSON 报告、2x2 对比图、局部放大、差异高亮与全局差异掩膜**（用最终代码 `--auto-size` 重新生成：PMX 400×520→520×520、Live2D 400×520→453×520，六项判定全部 PASS）
  - [pmx_compare.png](verify/feature/pmx_compare.png) / [live2d_compare.png](verify/feature/live2d_compare.png) —— 2x2 全景对比
  - [live2d_diff_expression.png](verify/feature/live2d_diff_expression.png) / [live2d_diff_lipsync.png](verify/feature/live2d_diff_lipsync.png) —— 变化像素红色高亮（眼眉 / 嘴部）
  - [pmx_diffmap.png](verify/feature/pmx_diffmap.png) / [live2d_diffmap.png](verify/feature/live2d_diffmap.png) —— 全局差异掩膜
  - [pmx_report.json](verify/feature/pmx_report.json) / [live2d_report.json](verify/feature/live2d_report.json) —— 像素与参数级判定结果
