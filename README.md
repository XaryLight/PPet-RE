# PPet（C++ 重写版）

> 基于 [zenghongtu/PPet](https://github.com/zenghongtu/PPet) 思路，用 **C++17 + OpenGL/GLFW** 从零重写的桌面宠物。
> 保留「透明置顶窗口 + 系统托盘 + 鼠标跟随」的桌面挂件体验，并带来三大增强：
> **全屏追踪**、**VRM 模型支持**、**PMX 模型支持**。

本项目是原生桌面程序（无 Electron、无浏览器运行时），启动快、内存占用低，并能在全屏游戏/应用中持续跟随光标。

---

## 特性

- **全屏追踪（Fullscreen Tracking）**：通过操作系统级 API 获取**整个虚拟桌面**的光标位置，模型眼睛/头部不再局限于自身窗口内跟随鼠标（解决原版 issue #26 / #122 / #158）。
- **多模型格式**：
  - **VRM 1.0 / 0.x**（glTF 容器，MToon 卡通渲染、表情 morph、弹簧骨、LookAt 视线追踪）
  - **PMX 2.0 / 2.1**（顶点蒙皮、骨骼、材质、表情变形）
  - **Live2D**（可选后端，见下文）
- **透明置顶窗口**：真·透明叠加层，支持点击穿透、窗口拖动、缩放。
- **系统托盘菜单**：置顶 / 忽略点击 / 显示工具栏 / 切换模型 / 语言 / 开机启动 / 反馈 / 关于 / 退出。
- **设置面板（ImGui）**：模型管理、渲染缩放、全屏追踪参数、语言切换。
- **中英双语**：界面文案随托盘菜单的语言选项切换。
- **开机启动**：跨平台注册表 / LaunchAgent / autostart 桌面项。
- **健壮的解析器**：所有模型文件读取都做了边界检查与索引越界防护，并配套 ASAN 模糊测试。

---

## 模型格式支持

| 格式 | 扩展名 | 状态 | 说明 |
|------|--------|------|------|
| VRM 1.0 | `.vrm` | ✅ 默认支持 | GLB/glTF2.0，含 MToon、humanoid、expressions、springBone |
| VRM 0.x | `.vrm` | ✅ 默认支持 | `extensions.VRM` 单体扩展，向后兼容 |
| PMX 2.0/2.1 | `.pmx` | ✅ 默认支持 | 二进制，含骨骼蒙皮、材质、变形、显示帧 |
| Live2D | `.model3.json` | ⚙️ 可选 | 需自行下载 Cubism SDK 并开启编译开关（专有许可） |

> 三种格式在内部统一转换为「蒙皮网格 + toon 着色」表示，共用同一套渲染管线。

---

## 构建

### 依赖

- **编译器**：支持 C++17 的 GCC / Clang / MSVC
- **构建系统**：CMake ≥ 3.16
- **图形**：OpenGL（桌面 OpenGL 3.3 核心）、[GLFW](https://www.glfw.org/)、[GLEW](https://glew.sourceforge.net/)
- **界面**：[Dear ImGui](https://github.com/ocornut/imgui)（源码已随仓库提供，无需另行安装）

#### Linux（Ubuntu / Debian）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
    libglfw3-dev libglew-dev libgl1-mesa-dev libglu1-mesa-dev \
    libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
    libxext-dev libxft-dev libfontconfig1-dev libssl-dev
# 可选：无头环境下跑图形测试
sudo apt-get install -y xvfb
```

#### Windows

使用 vcpkg 或 MSYS2 安装 `glfw3`、`glew`，并准备 Visual Studio（含 C++ 桌面开发负载）。

```powershell
vcpkg install glfw3 glew
```

#### macOS

```bash
brew install cmake glfw glew
```

### 编译步骤

```bash
git clone https://github.com/XaryLight/PPet-RE.git
cd PPet-RE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

产物：

- `build/bin/ppet` —— 主程序
- `build/bin/ppet_*_test` / `build/bin/ppet_*_smoke` —— 测试程序

### 启用 Live2D 后端（可选）

Live2D Cubism SDK 为**专有许可软件，不可随本仓库分发**。若要支持 Live2D 模型：

1. 从 <https://www.live2d.com/en/sdk/download/> 下载 **Cubism SDK for Native**；
2. 将 `Core` 目录放到 `third_party/Cubism/Core/`；
3. 在 CMake 中开启宏 `PPET_WITH_CUBISM` 后重新构建（Live2D 后端代码已随仓库提供，位于 `src/model/live2d/`）。

未启用时，加载 Live2D 模型会给出友好提示，不影响其它功能。

---

## 使用

### 运行

```bash
./build/bin/ppet
```

首次启动会加载默认配置并显示设置面板。右键系统托盘图标可打开菜单：

- **置顶 / 忽略点击 / 工具栏**：切换窗口行为
- **上一个模型 / 下一个模型**：在已导入模型间切换
- **语言**：简体中文 / English
- **配置**：打开设置面板
- **开机启动**：注册/取消自启
- **反馈**：打开 GitHub Issues
- **退出**

### 设置面板

点击托盘菜单「配置」打开 ImGui 设置窗口：

![设置面板](docs/settings-panel.png)

- **模型列表**：显示已导入模型与类型徽章，可切换 / 删除
- **导入模型**：从本地选择 `.vrm` / `.pmx` 文件
- **导入在线模型**：输入模型 URL
- **渲染大小**：0.25× ~ 4.0×
- **全屏追踪**：开关 + 追踪半径 + 最大偏转角度
- **窗口置顶 / 忽略鼠标点击 / 显示工具栏**
- **语言**、**清除设置**

### 全屏追踪

开启后，模型会跟随**整个屏幕（含多显示器）**的光标朝向，而不只是在窗口内。

- **追踪半径**（`trackingRadius`，像素）：光标距模型该距离时朝向达到最大偏转；值越大，跟随越「迟钝」。
- **最大偏转角度**（`maxTrackingAngle`，度）：头部/眼球的最大转动幅度。

实现方式（按平台）：

| 平台 | 全局光标获取 |
|------|--------------|
| Windows | `SetWindowsHookEx(WH_MOUSE_LL)` 低级鼠标钩子 |
| macOS | `NSEvent` 全局监视器（AppKit） |
| Linux | X11 `XRecord` 扩展；不可用时退化为轮询 `XQueryPointer` |

### 配置

配置以 JSON 持久化在用户配置目录（如 Linux 上为 `~/.config/PPet/ppet.json`）。常用字段：

| 字段 | 含义 | 默认 |
|------|------|------|
| `fullscreenTracking` | 是否启用全屏追踪 | `true` |
| `trackingRadius` | 追踪半径（像素） | `400` |
| `maxTrackingAngle` | 最大偏转角度（度） | `30` |
| `renderScale` | 渲染缩放 | `1.0` |
| `alwaysOnTop` | 窗口置顶 | `true` |
| `ignoreMouseEvents` | 点击穿透 | `false` |
| `showTool` | 显示工具栏 | `true` |
| `openAtLogin` | 开机启动 | `false` |
| `language` | 界面语言 | `Chinese` |
| `modelList` | 已导入模型列表 | `[]` |

### 调试与日志

程序运行时的所有调试信息都会写入当前工作目录下的 **`./last.log`**（UTF-8 编码，带 BOM），
用任意编辑器打开即可看到正常的中文，不再出现「灏氭湭鍔犺浇妯″瀷」这类乱码。

- 桌宠是透明无边框窗口，**未加载模型时它本身就是完全不可见的**——这是预期行为，
  不是 bug。首次启动或检测到「没有已配置模型」时会自动弹出设置面板，请在里面导入 `.vrm` / `.pmx` / `.json` 模型。
- 托盘菜单里的「调试」（<kbd>Ctrl</kbd>+<kbd>D</kbd>）会在日志中打印桌面尺寸、追踪状态与当前模型。
- Windows 控制台若仍显示乱码，是因为终端按 GBK 解释了 UTF-8 字节；直接查看 `./last.log` 即可看到正确内容。

---

## 项目结构

```
PPet-RE/
├── CMakeLists.txt            # 构建配置（模型层 / 平台层 / 主程序 / 测试）
├── src/
│   ├── core/                 # 数学库、配置、国际化
│   ├── platform/             # 窗口、托盘、全局光标追踪、开机启动、文件对话框
│   ├── model/                # 模型抽象 + 各后端
│   │   ├── SkinnedMesh.*     # 统一蒙皮网格表示
│   │   ├── pmx/              # PMX 2.0/2.1 解析与渲染
│   │   ├── vrm/              # GLB/glTF + VRM 扩展解析与渲染
│   │   ├── live2d/           # Live2D Cubism 后端（可选）
│   │   └── Model.*           # 模型工厂 / 类型探测
│   ├── render/               # toon 着色渲染管线、相机、工具
│   ├── ui/                   # ImGui 设置面板
│   ├── app/                  # 主循环与应用编排
│   └── main.cpp              # 程序入口
├── third_party/              # nlohmann/json、stb、imgui（源码随仓库）
└── tools/                    # 测试素材生成脚本与测试程序
```

---

## 测试

构建后运行完整测试套件：

```bash
cd build
ctest --output-on-failure
```

包含：

- **PMX / VRM 解析单元测试**：字段断言、UTF-8/UTF-16 文本、权重、骨骼层级
- **模糊测试（ASAN）**：截断 / 随机字节 / 畸形 JSON / 越界索引 —— 解析器应返回错误而非崩溃
- **渲染冒烟测试**：真实 GL 上下文下编译着色器、绘制网格、`glReadPixels` 校验非零像素
- **端到端集成测试**：真实模型文件 → 后端 → 渲染层 → 截图
- **设置界面冒烟测试**：独立窗口 + ImGui 初始化、中文字体合并、连续渲染、像素校验、可见性切换

测试素材由脚本生成：

```bash
python3 tools/gen_test_pmx.py
python3 tools/gen_test_vrm.py
```

---

## 致谢

- 原始项目：[zenghongtu/PPet](https://github.com/zenghongtu/PPet)（Electron + Live2D 桌面宠物）
- [Live2D Cubism SDK](https://www.live2d.com/)（专有许可，可选后端）
- [Dear ImGui](https://github.com/ocornut/imgui)、[GLFW](https://www.glfw.org/)、[GLEW](https://glew.sourceforge.net/)、[nlohmann/json](https://github.com/nlohmann/json)、[stb](https://github.com/nothings/stb)

---

## 许可证

Apache License 2.0，见 [LICENSE](./LICENSE)。

> 注意：Live2D Cubism SDK 采用其自有许可，不在本仓库许可范围内；使用 Live2D 后端时请遵守 Cubism SDK 的许可条款。
