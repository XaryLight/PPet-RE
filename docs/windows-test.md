# Windows 11 真机验收（T1）

本页对应验收要求：**Windows 11 + NVIDIA 显卡 + llvm-mingw 2026 ucrt + Qt 6.x + Live2D Cubism SDK 5.x**，
验证「全编译 → 试运行 → 添加 VRM / PMX(.zip) / moc3 模型 → 渲染窗口截图是否正常渲染」。

## 关于验证边界（请先读）

本仓库的开发环境是 **Linux 沙箱**，无法真实运行 Windows 11 桌面、NVIDIA 驱动与 Qt 窗口截图。
因此交付分两层：

| 层 | 谁来做 | 产出 |
| --- | --- | --- |
| **可自动化的等价验证** | 本仓库（Linux 离屏 GL + MinGW 交叉编译） | [`verify/`](verify/) 下三类模型的真实渲染 PNG 与 JSON 报告；Linux 三配置 0 警告构建 + 完整 ctest；`x86_64-w64-mingw32-g++ -std=c++23` 交叉语法检查 |
| **真机验收** | 你在 Windows 11 + N 卡机器上跑 | [`tools/windows_verify.ps1`](../tools/windows_verify.ps1) 一键脚本，产出 `verify_report/`（截图 + JSON + 汇总） |

渲染链路（模型加载 → 更新 → 绘制 → `glReadPixels` 判定）是同一份代码，Linux 离屏结果能证明「模型确实被画出来了」；
GPU 型号、驱动、Qt 平台插件、DLL 部署这些必须在真机上确认，脚本会把 `GL_RENDERER` 写进 JSON 报告。

## 1. 环境清单

| 组件 | 要求 | 备注 |
| --- | --- | --- |
| 操作系统 | Windows 11 | 透明置顶窗口依赖 DWM |
| 显卡 | NVIDIA（N 系列） | 驱动需支持 OpenGL 3.3 Core；报告里会打印 `GL_RENDERER` 供确认 |
| 编译器 | llvm-mingw 2026 **ucrt** x86_64（`clang++.exe`） | 必须 ucrt 版本；链接器是 `ld.lld` |
| Qt | Qt 6.x `llvm-mingw_64` | 与编译器 ABI 必须一致，混用 MSVC 版会链接失败 |
| CMake | ≥ 3.20 | C++23 需要 |
| Ninja | 任意新版 | generator 默认 Ninja |
| Cubism SDK | **for Native 5.x**（Core 6.0.1） | 把 `Core` 放到 `api/live2d/csdk/Core/` |
| PowerShell | 7+ | 运行验收脚本 |

> **SDK 放置**：`api/live2d/csdk/Core/include/Live2DCubismCore.h` 存在即可被自动识别。
> 旧位置 `src/csdk/`、`third_party/Cubism/Core/` 仍兼容，也可用 `-DPPET_CUBISM_ROOT=<Core 目录>` 指定。
> SDK 是专有许可，已被 `.gitignore` 排除，不会随仓库分发。

## 2. 一键验收

在仓库根目录打开 PowerShell：

```powershell
.\tools\windows_verify.ps1 `
    -QtPrefix    "C:/Qt/6.7.2/llvm-mingw_64/lib/cmake" `
    -CubismRoot  "D:/PPet/api/live2d/csdk/Core" `
    -Vrm         "D:/models/test.vrm" `
    -PmxZip      "D:/models/Model.zip" `
    -Moc3        "D:/models/Hiyori/Hiyori.model3.json"
```

脚本依次执行：

1. **configure** — `-DPPET_WITH_CUBISM=ON`，打印 `Live2D 后端：启用真实 Cubism SDK (...)`；
2. **build** — 全量编译，含 `ppet.exe` 与 `ppet_render_verify.exe`；
3. **windeployqt** — 部署 Qt 运行时与 `platforms/qwindows.dll`，并核对 `Live2DCubismCore.dll` / MinGW 运行时 DLL；
4. **试运行** — 启动 `ppet.exe` 数秒，检查是否闪退，并扫描 `last.log` 的 ERROR/FATAL；
5. **渲染验证** — 三类模型各跑一次 `ppet_render_verify.exe`，产出 PNG + JSON；
6. **附加验收：清屏契约回归** — 用其中一个模型跑「每帧清屏 / 漏清屏」两次，期望前者覆盖率 > 1% 且 PASS、后者覆盖率 0% 且 FAIL，结果写入 `summary.json` 的 `clear_contract`。

参数：

| 参数 | 说明 |
| --- | --- |
| `-QtPrefix` | Qt6 的 `lib/cmake` 路径；用于 `CMAKE_PREFIX_PATH` 与定位 `windeployqt` |
| `-CubismRoot` | Cubism SDK 的 `Core` 目录（可选，自动探测时会忽略） |
| `-Toolchain` | CMake toolchain 文件（CLion 用了自定义工具链时传） |
| `-Generator` | 默认 `Ninja` |
| `-Vrm` / `-PmxZip` / `-Moc3` | 三类模型路径，留空则跳过该项并在报告标 `SKIPPED` |
| `-OutDir` | 报告目录，默认 `verify_report/` |
| `-SkipBuild` | 已编译过、只需重跑验证时使用 |

## 3. 手动等价步骤

不用脚本时，逐条执行：

```powershell
# configure（C++23 + Qt6 + Live2D SDK 5.x）
cmake -S . -B cmake-build-verify -G Ninja `
      -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH="C:/Qt/6.7.2/llvm-mingw_64/lib/cmake" `
      -DPPET_WITH_CUBISM=ON

# 全编译
cmake --build cmake-build-verify --parallel

# 部署 Qt 运行时（否则启动报 0xC0000135 或 "could not find Qt platform plugin"）
& "C:/Qt/6.7.2/llvm-mingw_64/bin/windeployqt.exe" `
    --no-translations --no-system-d3d-compiler --no-opengl-sw `
    cmake-build-verify/bin/ppet.exe

# 试运行：直接双击 ppet.exe，或用下面的渲染验证工具做无人值守验证
.\cmake-build-verify\ppet_render_verify.exe --model "D:/models/test.vrm" `
    --shot verify/vrm.png --report verify/vrm.json --frames 30
.\cmake-build-verify\ppet_render_verify.exe --model "D:/models/Model.zip" `
    --shot verify/pmx_zip.png --report verify/pmx_zip.json --frames 30
.\cmake-build-verify\ppet_render_verify.exe --model "D:/models/Hiyori/Hiyori.model3.json" `
    --shot verify/moc3.png --report verify/moc3.json --frames 30
```

`ppet_render_verify` 的参数见 [`--help`](../tools/render_verify.cpp)；
它会把 premultiplied-alpha 的渲染结果合成到棋盘背景，PNG 可直接肉眼核验「模型在不在、大不大、正不正」。

## 4. 判定标准

每份 `*.json` 的关键字段：

| 字段 | 期望 |
| --- | --- |
| `result` | `PASS` / `PASS_WITH_WARNINGS` 为通过；`FAIL` 为不通过 |
| `render.coverage_percent` | 非透明像素占比 > `0.2%`（可用 `--min-coverage` 调整） |
| `render.gl_error` | `0` |
| `render.gpu_ready` / `rendering_ok` | `true` |
| `render.model_status` | 「正常渲染」 |
| `render.clear_per_frame` | `true`（验收工具固定每帧清屏） |
| `render.frame_coverage` | 用 `--swap-each-frame` 时逐帧覆盖率应稳定，不能出现某一帧掉到 0 |
| 清屏契约检查 | 脚本会用同一模型跑一次 `--no-clear --swap-each-frame`，期望覆盖率 0% 且 `result=FAIL`；若变成 PASS 说明复现失效，需复核 |
| `model.bones` | PMX/VRM > 0；Live2D 为 0（二维部件管线，属正常） |
| `model.missing_resources` | 空；非空会进 `warnings`（PMX 常见白模原因） |
| `platform.gl_renderer` | 应显示 NVIDIA 显卡，用于确认走的是独显而非 llvmpipe/软件渲染 |

`summary.json` 的 `overall` 为 `PASS` 才算整体验收通过。

## 5. 常见故障

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `ld.lld: could not open 'libLIBCMT.a' / 'libOLDNAMES.a'` | 链接了 MSVC 静态库 | 新 CMake 在 MinGW 下**强制**走 `dll/windows/x86_64/Live2DCubismCore.lib` 导入库，并在配置阶段直接报错说明；确认 SDK 的 `dll/` 目录完整 |
| 退出码 `3221225781 (0xC0000135)` | 缺 DLL | 跑 `windeployqt`；确认 `Live2DCubismCore.dll`、`libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll` 与 exe 同目录 |
| `could not find or load the Qt platform plugin "windows"` | Qt 插件未部署 | `windeployqt` 会生成 `platforms/qwindows.dll` |
| `0xC0000135` 但 DLL 都在 | ABI 混用（MSVC 版 Qt + llvm-mingw 编译器） | Qt 必须选 `llvm-mingw_64` 套件 |
| `-DPPET_WITH_CUBISM=OFF` 仍启用 Live2D | 旧版本被自动检测 `FORCE` 覆盖 | 已修复：显式设置优先；`git pull` 后删除 `cmake-build-*` 重新 configure |
| 模型能加载但白模 | PMX 单文件导入，贴图没跟上 | 把 `.pmx` 与 `tex/` 一起打包成 `.zip` 再导入；`missing_resources` 会列出缺哪些 |
| 诊断面板「正常渲染」、GL 错误 0、Draw calls 正常，但屏幕没模型 | 主循环漏了每帧清屏，深度缓冲残留导致片元全被剔除 | `git pull` 到最新版（已修复）；打开诊断面板看「实际出图 Pixels」是否为 `0.00% ← 未出图`；见 [build.md](build.md#运行期诊断面板全绿屏幕却看不到模型) |
| 弹「Live2D 后端未启用」 | 没开 `PPET_WITH_CUBISM` | 检查 configure 输出有无「启用真实 Cubism SDK」 |
| `GL_RENDERER` 显示 llvmpipe / Microsoft Basic Render | 用了软件渲染 | NVIDIA 控制面板里把 `ppet.exe` 指定为高性能 GPU |

## 6. 手动 UI 验证清单（自动化覆盖不到的部分）

1. 托盘右键 → **模型诊断**（`Ctrl+Shift+D`），确认六个页签都有内容：
   - 概览与渲染（模型 ID、状态灯、FPS、GL 错误）——**重点核对「实际出图 Pixels」> 0 且状态灯为绿**；这一行是帧缓冲读回的真实像素证据，比 draw calls / GL 错误更能说明「真的画出来了」
   - 参数（顶点/三角形/骨骼/贴图）
   - **骨架信息**：逐根骨骼 ID、名称、父级、层级、属性
   - **图片渲染**：贴图缩略图 + 当前渲染帧实时预览
   - 模型部分（子网格/材质）
   - 性质与资源（含详细错误与警告）
2. 托盘 → **导入模型**，故意选一个损坏的 `.pmx` 或不配套的 `.moc3`，确认弹出失败提示框，
   且显示分类（解析错误 / moc3 模型错误 / 模型图片缺失）、逐条明细、建议与日志路径。
3. 单独导入一个没有贴图的 `.pmx`，确认弹出**警告**提示（「模型图片缺失」+ 打包成 `.zip` 的建议）。
4. 切换中英文，确认弹窗与诊断面板文案同步更新。

## 7. Linux 离屏等价证据

仓库里的 [`verify/`](verify/) 目录由同一份 `ppet_render_verify` 在 Linux + `xvfb-run` + Mesa llvmpipe 下生成：

| 文件 | 内容 |
| --- | --- |
| `verify/vrm.png` / `.json` | VRM 模型渲染截图与报告 |
| `verify/pmx_zip.png` / `.json` | PMX `.zip` 整包导入（含贴图）渲染截图与报告 |
| `verify/moc3.png` / `.json` | Cubism SDK 5-r.5 官方 Hiyori `.moc3` 模型渲染截图与报告 |

生成命令（Linux）：

```bash
xvfb-run -a ./build/ppet_render_verify --model <模型> \
    --shot docs/verify/<name>.png --report docs/verify/<name>.json --frames 30
```

> 注意：Linux 报告的 `gl_renderer` 是 `llvmpipe`（软件渲染），
> 它证明的是渲染链路正确；**NVIDIA 硬件渲染仍需在 Windows 真机按第 2 节跑一遍**。
