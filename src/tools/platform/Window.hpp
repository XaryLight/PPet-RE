// PPet - C++ 移植版
// 平台窗口层：GLFW 窗口 + 各平台的透明 / 置顶 / 点击穿透 / 全屏独占置顶维持。
//
// 对应原 Electron 版的 BrowserWindow 能力（见原项目 src/main/tray.ts 与 window.ts）：
//   - transparent: true, frame: false  → 透明无边框
//   - alwaysOnTop                    → 置顶
//   - setIgnoreMouseEvents(true, { forward: true }) → 点击穿透
//   - 拖动 / 缩放由渲染层与本层的位置尺寸接口配合完成
//
// 平台差异集中在 .cpp 内用 #if defined(_WIN32) / __APPLE__ 分离，接口保持一致。
#pragma once

#include <functional>
#include <string>

#include <src/tools/core/Math.hpp>

struct GLFWwindow;

namespace ppet {

/// 窗口创建选项。
struct WindowOptions {
    int width = 350;
    int height = 600;
    int x = 100;
    int y = 100;
    std::string title = "PPet";
    bool alwaysOnTop = true;
    bool transparent = true;
    bool resizable = false;
    bool skipTaskbar = true;
};

/// 平台窗口。
///
/// 坐标约定：Position()/SetPosition() 使用「虚拟桌面坐标」——
///   原点位于虚拟桌面左上角，X 向右递增，Y 向下递增；
///   多显示器时坐标可以为负（表示位于主屏左侧 / 上方的显示器）。
/// 这与 GlobalCursorTracker 的坐标系保持一致，两者可以直接相减得到相对偏移。
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// 创建 GLFW 窗口与 OpenGL 3.3 核心上下文。
    /// 失败时返回 false，error 中写入中文原因（不会崩溃、不会抛异常）。
    bool Create(const WindowOptions& options, std::string& error);

    /// 销毁窗口并释放平台资源。可重复调用。
    void Destroy();

    bool ShouldClose() const;

    /// 请求关闭窗口（等价于用户点击关闭按钮），会触发 onClose 回调。
    void RequestClose();
    void SwapBuffers();
    void PollEvents();

    void SetAlwaysOnTop(bool on);
    void SetIgnoreMouseEvents(bool ignore);
    void SetTransparent(bool transparent);
    void SetTitle(const std::string& title);

    /// 窗口位置与尺寸（虚拟桌面坐标）。
    Vec2 Position() const;
    Vec2 Size() const;
    void SetPosition(int x, int y);
    void SetSize(int width, int height);

    /// 每帧调用：处理全屏应用置顶维持等平台逻辑。
    ///
    /// Windows 上独占全屏的游戏在切换显示模式时会重置 Z 序，把普通置顶窗口压到后面；
    /// 这里检测到「前台窗口覆盖了整个显示器」时，会周期性地把本窗口重新提到顶层，
    /// 这是 DirectComposition 之外最常用的折中做法。
    void UpdatePlatformState();

    GLFWwindow* Handle() const { return window_; }

    // -------------------------------------------------------------------------
    // 状态查询（供 UI 层判断当前生效状态，不属于强制契约但很实用）
    // -------------------------------------------------------------------------
    bool IsAlwaysOnTop() const { return alwaysOnTop_; }
    bool IsIgnoreMouseEvents() const { return ignoreMouseEvents_; }
    bool IsTransparent() const { return transparent_; }

    /// 是否检测到「独占全屏的前台应用」（此时本窗口可能被游戏画面盖住）。
    bool IsFullscreenHostActive() const { return fullscreenHostActive_; }

    /// 平台降级说明（中文）。例如 Linux 上透明依赖合成器、点击穿透依赖 XShape。
    /// 空字符串表示没有降级。
    const std::string& PlatformNote() const { return platformNote_; }

    // -------------------------------------------------------------------------
    // 回调
    // -------------------------------------------------------------------------
    std::function<void(int key, int scancode, int action)> onKey;
    std::function<void(double xpos, double ypos)> onMouseMove;
    std::function<void()> onMouseEnter;
    std::function<void()> onMouseLeave;
    std::function<void(int button, int action)> onMouseButton;
    std::function<void(double dx, double dy)> onScroll;
    /// 窗口尺寸变化回调。
    /// 参数是**帧缓冲的实际像素尺寸**（HiDPI 下与窗口逻辑尺寸不同），
    /// 渲染层可直接用它设置 glViewport。
    std::function<void(int width, int height)> onResize;
    std::function<void()> onClose;

private:
    // 各平台的附加初始化（创建窗口后调用）。
    void ApplyPlatformOptions(const WindowOptions& options);

    /// 初始化 OpenGL 上下文与 GLEW。
    /// GLFW 创建窗口后上下文默认不绑定到任何线程，必须先 glfwMakeContextCurrent
    /// 再 glewInit，否则所有 GL/GLEW 调用都作用在「无上下文」上（函数指针为空），
    /// 导致渲染层什么也画不出来。core profile 下必须先把 glewExperimental 打开。
    /// 仅在第一次创建成功时调用一次。
    bool InitGL(std::string& error);

    // 各平台每帧逻辑。
    void UpdatePlatformStateImpl();

    // GLFW 的回调是 C 风格函数指针（没有 userdata），
    // 因此用静态成员函数做跳板，从 glfwGetWindowUserPointer 取回实例。
    static void GlfwKeyCallback(GLFWwindow* handle, int key, int scancode, int action,
                                 int mods);
    static void GlfwCursorPosCallback(GLFWwindow* handle, double xpos, double ypos);
    // GLFW 3.3 的进入/离开回调是同一个：entered == 1 表示进入，0 表示离开。
    static void GlfwCursorEnterCallback(GLFWwindow* handle, int entered);
    static void GlfwMouseButtonCallback(GLFWwindow* handle, int button, int action, int mods);
    static void GlfwScrollCallback(GLFWwindow* handle, double dx, double dy);
    static void GlfwWindowSizeCallback(GLFWwindow* handle, int width, int height);
    static void GlfwFramebufferSizeCallback(GLFWwindow* handle, int width, int height);
    static void GlfwWindowCloseCallback(GLFWwindow* handle);

    GLFWwindow* window_ = nullptr;

    bool alwaysOnTop_ = false;
    bool ignoreMouseEvents_ = false;
    bool transparent_ = false;
    bool fullscreenHostActive_ = false;

    /// 平台能力降级说明。
    std::string platformNote_;

    /// 记录上一次置顶维持的时间戳（Windows / Linux 用），避免每帧调用系统 API。
    double lastTopmostReassert_ = 0.0;

    /// 上一次做「前台窗口是否独占全屏」检测的时间戳。
    /// GetForegroundWindow / GetWindowRect / GetMonitorInfoW 是跨进程调用，
    /// 前台窗口卡死或第三方 overlay 挂住窗口枚举时会阻塞渲染主线程。
    double lastFullscreenCheck_ = 0.0;

    /// Linux 下 XShape 是否可用；不可用时点击穿透退化为「只屏蔽自身事件处理」。
    bool nativeClickThrough_ = false;

    /// 窗口创建时记录的初始尺寸，用于 Linux 上恢复点击穿透时重建输入区域。
    int lastWidth_ = 0;
    int lastHeight_ = 0;

    /// OpenGL 上下文与 GLEW 是否已经初始化（仅首次创建窗口时做一次）。
    bool glReady_ = false;
};

}  // namespace ppet
