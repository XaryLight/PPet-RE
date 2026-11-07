// PPet - C++ 移植版
// 平台窗口层实现。
//
// 设计要点：
//   1. 跨平台部分（创建、事件回调、尺寸位置）完全走 GLFW；
//   2. 平台差异（真透明、点击穿透、置顶层级、全屏独占置顶维持）用 #if 分离，
//      每个分支都提供降级方案，绝不因为平台能力缺失而崩溃；
//   3. 坐标统一使用「虚拟桌面坐标」（原点在虚拟桌面左上角，Y 向下，可为负），
//      与 GlobalCursorTracker 完全一致。
#include <src/tools/platform/Window.hpp>

#include <chrono>
#include <cstdio>
#include <string>

// 原生句柄接口（glfwGetWin32Window / glfwGetCocoaWindow / glfwGetX11Window）位于
// glfw3native.h，必须在 glfw3.h 之后包含，并声明要暴露的窗口后端。
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#endif

// GLEW 必须在 GLFW 之前包含：glfw3.h 内部会引入 GL/gl.h，而 glew.h 要求自己先于
// 任何 GL 头文件被包含，否则报 “gl.h included before glew.h”。
#include <GL/glew.h>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>

// 老版本 SDK 没有这个常量（Win10 2004 引入），手工补上避免编译失败。
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#elif defined(__APPLE__)
// 本文件在 Apple 平台需要以 Objective-C++ 编译（CMake 中通过
// set_source_files_properties(... LANGUAGE OBJCXX) 指定）。
#import <AppKit/AppKit.h>

#else
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#endif

namespace ppet {

#if !defined(_WIN32) && !defined(__APPLE__)
/// X11 的窗口句柄类型叫 Window，与本项目的 ppet::Window 同名，
/// 这里取别名避免在 ppet 命名空间内产生歧义。
using XWindow = ::Window;
#endif

namespace {

/// GLFW 上一次报错的描述。
std::string g_lastGlfwError;

/// 进程内窗口计数：GLFW 的初始化/终止是全局状态，
/// 只有最后一个窗口销毁时才 glfwTerminate，避免误伤同进程的其他窗口。
int g_windowCount = 0;

void GlfwErrorCallback(int code, const char* description) {
    g_lastGlfwError = "GLFW 错误码 " + std::to_string(code) + "：" +
                      (description ? description : "未知错误");
}

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#if !defined(_WIN32) && !defined(__APPLE__)

/// X11 错误吞掉器：桌宠不应该因为一次 X 协议错误（例如窗口已被销毁）而整个退出。
/// 只在调用可能出错的 X 扩展接口时临时安装，调用完立即还原。
int g_xErrorCount = 0;

int IgnoreXError(Display* /*display*/, XErrorEvent* /*event*/) {
    ++g_xErrorCount;
    return 0;
}

/// 临时安装 X 错误处理器的 RAII 辅助。
class ScopedXErrorGuard {
public:
    ScopedXErrorGuard() : previous_(XSetErrorHandler(IgnoreXError)) {}
    ~ScopedXErrorGuard() { XSetErrorHandler(previous_); }
    ScopedXErrorGuard(const ScopedXErrorGuard&) = delete;
    ScopedXErrorGuard& operator=(const ScopedXErrorGuard&) = delete;

private:
    XErrorHandler previous_;
};

/// 查询 XShape 扩展是否可用（不产生 X 错误）。
bool X11HasShape(Display* display) {
    int eventBase = 0;
    int errorBase = 0;
    return XShapeQueryExtension(display, &eventBase, &errorBase) == True;
}

/// 通过 _NET_WM_STATE_ABOVE 设置/取消置顶。
///
/// GLFW 的 GLFW_FLOATING 在 X11 上最终也是走这条 EWMH 协议，这里再手写一遍是为了
/// 兼容那些不处理 GLFW 私有路径的窗口管理器。
void X11SetAbove(Display* display, XWindow window, bool above) {
    if (!display || !window) return;
    ScopedXErrorGuard guard;
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format = 32;
    // 1 = _NET_WM_STATE_ADD，0 = _NET_WM_STATE_REMOVE
    event.xclient.data.l[0] = above ? 1 : 0;
    event.xclient.data.l[1] = static_cast<long>(XInternAtom(display, "_NET_WM_STATE_ABOVE", False));
    event.xclient.data.l[2] = 0;
    // 1 = 来自普通应用程序的状态切换
    event.xclient.data.l[3] = 1;
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

/// 用 XShape 的「输入区域」实现点击穿透。
///
/// 原理：把窗口的 ShapeInput 区域设为空矩形，X 服务器就不再把该窗口当作命中目标，
/// 鼠标事件会直接穿透到下层窗口。这是 X11 上实现「点击穿透」的标准做法。
///
/// @param ignore true = 设为空区域（穿透）；false = 恢复为整个窗口区域。
/// @return 是否成功（XShape 不可用时返回 false）。
bool X11SetInputRegion(Display* display, XWindow window, int width, int height, bool ignore) {
    if (!display || !window) return false;
    if (!X11HasShape(display)) return false;

    ScopedXErrorGuard guard;
    if (ignore) {
        // 传入 0 个矩形 => 空输入区域 => 窗口不参与命中测试。
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0, nullptr, 0, ShapeSet,
                                Unsorted);
    } else {
        XRectangle rect{};
        rect.x = 0;
        rect.y = 0;
        rect.width = static_cast<unsigned short>(width > 0 ? width : 1);
        rect.height = static_cast<unsigned short>(height > 0 ? height : 1);
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0, &rect, 1, ShapeSet,
                                Unsorted);
    }
    XFlush(display);
    return g_xErrorCount == 0;
}

/// 设置窗口类型为「工具/启动画面」类型，尽量让窗口不出现在任务栏中。
///
/// EWMH 没有统一的「不进任务栏」属性，_NET_WM_WINDOW_TYPE_SPLASH 是各窗口管理器
/// 处理得最一致的一种（GNOME / KDE / XFCE 都不会把它列入任务栏）。
void X11SetWindowTypeUtility(Display* display, XWindow window) {
    if (!display || !window) return;
    ScopedXErrorGuard guard;
    const Atom typeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom splashAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    XChangeProperty(display, window, typeAtom, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&splashAtom), 1);
    XFlush(display);
}

#endif  // !_WIN32 && !__APPLE__

}  // namespace

// ---------------------------------------------------------------------------
// 创建 / 销毁
// ---------------------------------------------------------------------------

bool Window::Create(const WindowOptions& options, std::string& error) {
    if (window_) {
        error = "窗口已经创建，无法重复创建";
        return false;
    }

    g_lastGlfwError.clear();
    glfwSetErrorCallback(GlfwErrorCallback);

    if (glfwInit() != GLFW_TRUE) {
        error = "无法初始化 GLFW：当前环境没有可用的图形显示"
                "（Linux 下需要 DISPLAY 环境变量，例如 X11 或 XWayland 会话）";
        if (!g_lastGlfwError.empty()) error += "；" + g_lastGlfwError;
        return false;
    }

    // 上下文版本：优先 3.3 核心 profile（渲染层需要），失败后退回兼容 profile。
    struct Profile {
        int major;
        int minor;
        bool core;
    };
    const Profile profiles[] = {
        {3, 3, true},
        {3, 3, false},
        {2, 1, false},
    };

    for (const Profile& profile : profiles) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, profile.major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, profile.minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE,
                       profile.core ? GLFW_OPENGL_CORE_PROFILE : GLFW_OPENGL_ANY_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,
                       profile.core ? GLFW_TRUE : GLFW_FALSE);
        // 无边框：桌宠需要自定义外观，窗口装饰会破坏透明效果。
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, options.resizable ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER,
                       options.transparent ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_FLOATING, options.alwaysOnTop ? GLFW_TRUE : GLFW_FALSE);
#ifdef GLFW_SKIP_TASKBAR
        // GLFW 3.4+ 才有的提示；3.3 下由各平台分支自行处理（Windows 用 WS_EX_TOOLWINDOW，
        // X11 用 _NET_WM_WINDOW_TYPE）。
        glfwWindowHint(GLFW_SKIP_TASKBAR, options.skipTaskbar ? GLFW_TRUE : GLFW_FALSE);
#endif
        // 桌宠不应该在显示时抢走用户正在输入的窗口焦点。
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        window_ = glfwCreateWindow(options.width, options.height, options.title.c_str(),
                                   nullptr, nullptr);
        if (window_) break;
    }

    if (!window_) {
        error = "创建 GLFW 窗口失败";
        if (!g_lastGlfwError.empty()) error += "；" + g_lastGlfwError;
        glfwTerminate();
        return false;
    }

    lastWidth_ = options.width;
    lastHeight_ = options.height;
    alwaysOnTop_ = options.alwaysOnTop;
    ignoreMouseEvents_ = false;
    transparent_ = options.transparent;
    fullscreenHostActive_ = false;
    lastTopmostReassert_ = NowSeconds();

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, &Window::GlfwKeyCallback);
    glfwSetCursorPosCallback(window_, &Window::GlfwCursorPosCallback);
    glfwSetCursorEnterCallback(window_, &Window::GlfwCursorEnterCallback);
    glfwSetMouseButtonCallback(window_, &Window::GlfwMouseButtonCallback);
    glfwSetScrollCallback(window_, &Window::GlfwScrollCallback);
    glfwSetFramebufferSizeCallback(window_, &Window::GlfwFramebufferSizeCallback);
    glfwSetWindowSizeCallback(window_, &Window::GlfwWindowSizeCallback);
    glfwSetWindowCloseCallback(window_, &Window::GlfwWindowCloseCallback);

    // 先定位再显示，避免出现窗口先在 (0,0) 闪一下再跳到目标位置的情况。
    glfwSetWindowPos(window_, options.x, options.y);
    glfwShowWindow(window_);

    ApplyPlatformOptions(options);

    // 把 GL 上下文绑定到当前线程并初始化 GLEW：这是渲染能出图的前提，
    // 缺了这两步，渲染层的所有 GL 调用都会因为「无当前上下文」而失败。
    if (!InitGL(error)) return false;

    ++g_windowCount;
    return true;
}

void Window::Destroy() {
    if (!window_) return;
    glfwDestroyWindow(window_);
    window_ = nullptr;
    if (g_windowCount > 0) --g_windowCount;
    // 最后一个窗口销毁时才终止 GLFW，释放 X11/Wayland 连接。
    if (g_windowCount == 0) glfwTerminate();
}

Window::~Window() { Destroy(); }

// ---------------------------------------------------------------------------
// 帧循环
// ---------------------------------------------------------------------------

bool Window::ShouldClose() const {
    return window_ && glfwWindowShouldClose(window_) != GLFW_FALSE;
}

void Window::RequestClose() {
    if (window_) glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void Window::SwapBuffers() {
    if (window_) glfwSwapBuffers(window_);
}

void Window::PollEvents() {
    if (window_) glfwPollEvents();
}

void Window::UpdatePlatformState() { UpdatePlatformStateImpl(); }

// ---------------------------------------------------------------------------
// 状态设置
// ---------------------------------------------------------------------------

void Window::SetAlwaysOnTop(bool on) {
    alwaysOnTop_ = on;
    if (!window_) return;

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (hwnd) {
        // HWND_TOPMOST / HWND_NOTOPMOST 是 Win32 表达「置顶 / 取消置顶」的唯一方式。
        SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#elif defined(__APPLE__)
    NSWindow* ns = reinterpret_cast<NSWindow*>(glfwGetCocoaWindow(window_));
    if (ns) {
        // NSFloatingWindowLevel 高于普通窗口，低于系统面板与全屏辅助窗口。
        [ns setLevel:on ? NSFloatingWindowLevel : NSNormalWindowLevel];
    }
#else
    Display* display = glfwGetX11Display();
    XWindow xwin = glfwGetX11Window(window_);
    if (display && xwin) {
        glfwSetWindowAttrib(window_, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE);
        X11SetAbove(display, xwin, on);
    }
#endif
}

void Window::SetIgnoreMouseEvents(bool ignore) {
    ignoreMouseEvents_ = ignore;
    if (!window_) return;

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (hwnd) {
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        // WS_EX_TRANSPARENT 让本窗口不参与命中测试，点击直接穿透到下层窗口。
        if (ignore) {
            exStyle |= WS_EX_TRANSPARENT;
        } else {
            exStyle &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        }
        // 始终保持 LAYERED（逐像素透明）、TOOLWINDOW（不进任务栏）、NOACTIVATE（不抢焦点）。
        exStyle |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
        // 修改扩展样式后必须让窗口重算非客户区，否则穿透不会立即生效。
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#elif defined(__APPLE__)
    NSWindow* ns = reinterpret_cast<NSWindow*>(glfwGetCocoaWindow(window_));
    if (ns) {
        // ignoresMouseEvents 是 AppKit 原生的点击穿透开关。
        [ns setIgnoresMouseEvents:ignore ? YES : NO];
    }
#else
    Display* display = glfwGetX11Display();
    XWindow xwin = glfwGetX11Window(window_);
    if (display && xwin && nativeClickThrough_) {
        X11SetInputRegion(display, xwin, lastWidth_, lastHeight_, ignore);
    }
    // XShape 不可用时的降级由 GlfwCursorPosCallback 等回调负责：
    // 不再把鼠标事件派发给上层（渲染层因此不会响应点击），
    // 但窗口本身仍会吃掉事件，这是没有 XShape 时无法回避的固有限制。
#endif
}

void Window::SetTransparent(bool transparent) {
    transparent_ = transparent;
    if (!window_) return;

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (hwnd) {
        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (transparent) {
            // 不使用 WS_EX_LAYERED：分层窗口由 GDI 合成，会丢弃 OpenGL
            // 帧缓冲的 alpha 通道（窗口会变成全空白）。
            // 改用与 GLFW 一致的 DWM 模糊区域方案。
            exStyle &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
            HRGN region = CreateRectRgn(0, 0, -1, -1);
            DWM_BLURBEHIND bb = {};
            bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
            bb.hRgnBlur = region;
            bb.fEnable = TRUE;
            DwmEnableBlurBehindWindow(hwnd, &bb);
            DeleteObject(region);
        } else {
            DWM_BLURBEHIND bb = {};
            bb.dwFlags = DWM_BB_ENABLE;
            bb.fEnable = FALSE;
            DwmEnableBlurBehindWindow(hwnd, &bb);
            SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
                             reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#elif defined(__APPLE__)
    NSWindow* ns = reinterpret_cast<NSWindow*>(glfwGetCocoaWindow(window_));
    if (ns) {
        [ns setOpaque:transparent ? NO : YES];
        [ns setBackgroundColor:transparent ? [NSColor clearColor] : [NSColor blackColor]];
    }
#else
    // Linux 上 GLFW_TRANSPARENT_FRAMEBUFFER 只能在创建窗口时确定；
    // 运行期关闭透明需要重建窗口（会丢失 OpenGL 上下文），因此这里只记录状态并给出提示。
    if (!transparent) {
        platformNote_ = "Linux 上透明背景需在创建窗口时确定，运行期切换需要重建窗口";
    }
#endif
}

void Window::SetTitle(const std::string& title) {
    if (!window_) return;
    glfwSetWindowTitle(window_, title.c_str());
}

// ---------------------------------------------------------------------------
// 位置与尺寸
// ---------------------------------------------------------------------------

Vec2 Window::Position() const {
    if (!window_) return Vec2();

#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (hwnd) {
        // 【重要】ClientToScreen 的输入必须是「客户区坐标」，不是屏幕坐标。
        // 旧代码把 GetWindowRect 得到的屏幕坐标 rect.left/rect.top 又传进去做了一次
        // 客户区→屏幕换算，结果约为真实坐标的 2 倍；SaveConfig() 把这个值写回配置，
        // 下次启动窗口位置就翻倍，反复几次后窗口彻底跑出桌面（Win+Tab 能看到却够不着）。
        POINT origin{0, 0};
        if (ClientToScreen(hwnd, &origin)) {
            return Vec2(static_cast<float>(origin.x), static_cast<float>(origin.y));
        }
    }
    // hwnd 获取失败时回退到 GLFW 提供的窗口位置。
    int x = 0;
    int y = 0;
    glfwGetWindowPos(window_, &x, &y);
    return Vec2(static_cast<float>(x), static_cast<float>(y));
#elif defined(__APPLE__)
    int x = 0;
    int y = 0;
    glfwGetWindowPos(window_, &x, &y);
    return Vec2(static_cast<float>(x), static_cast<float>(y));
#else
    // Linux：直接用 X11 查询，坐标基于根窗口，因此天然支持负坐标与多显示器。
    Display* display = glfwGetX11Display();
    XWindow xwin = glfwGetX11Window(window_);
    if (display && xwin) {
        XWindow root = DefaultRootWindow(display);
        XWindow child = 0;
        int rootX = 0;
        int rootY = 0;
        if (XTranslateCoordinates(display, xwin, root, 0, 0, &rootX, &rootY, &child)) {
            return Vec2(static_cast<float>(rootX), static_cast<float>(rootY));
        }
    }
    int x = 0;
    int y = 0;
    glfwGetWindowPos(window_, &x, &y);
    return Vec2(static_cast<float>(x), static_cast<float>(y));
#endif
}

Vec2 Window::Size() const {
    if (!window_) return Vec2();
    int width = 0;
    int height = 0;
    glfwGetWindowSize(window_, &width, &height);
    return Vec2(static_cast<float>(width), static_cast<float>(height));
}

void Window::SetPosition(int x, int y) {
    if (!window_) return;
    glfwSetWindowPos(window_, x, y);
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (hwnd) {
        // SWP_NOZORDER 保证只改位置、不动 Z 序（置顶状态由 SetAlwaysOnTop 负责）。
        SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

void Window::SetSize(int width, int height) {
    if (!window_) return;
    glfwSetWindowSize(window_, width, height);
    lastWidth_ = width;
    lastHeight_ = height;
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (hwnd) {
        SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

// ---------------------------------------------------------------------------
// 平台初始化与每帧逻辑
// ---------------------------------------------------------------------------

void Window::ApplyPlatformOptions(const WindowOptions& options) {
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (!hwnd) {
        platformNote_ = "无法获取原生窗口句柄，透明与置顶功能不可用";
        return;
    }

    // 扩展样式：
    //   WS_EX_TOOLWINDOW—— 不出现在任务栏与 Alt-Tab 列表中
    //   WS_EX_NOACTIVATE—— 点击窗口不会抢走当前应用的输入焦点
    //
    // 注意：这里绝对不能加 WS_EX_LAYERED！
    // 带 WS_EX_LAYERED 的窗口由 GDI 的「分层窗口」合成路径接管，
    // DWM 不再读取 OpenGL 帧缓冲的 alpha 通道，结果就是
    // 「窗口存在但完全空白」——这是 Windows 上桌宠看不见的经典原因。
    // GLFW 自己实现逐像素透明用的是 DwmEnableBlurBehindWindow（见下），
    // 那条路径才与 OpenGL 帧缓冲兼容。
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (options.skipTaskbar) exStyle |= WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    if (options.transparent) {
        // 与 GLFW 的 Windows 透明实现保持一致：用空的模糊区域让 DWM
        // 合成器采用窗口自身的 alpha 通道，实现逐像素真透明。
        // （GLFW 3.3 的 win32_window.c 里 updateFramebufferTransparency 用的
        //   就是 DwmEnableBlurBehindWindow + 空 HRGN。）
        HRGN region = CreateRectRgn(0, 0, -1, -1);
        DWM_BLURBEHIND bb = {};
        bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        bb.hRgnBlur = region;
        bb.fEnable = TRUE;
        DwmEnableBlurBehindWindow(hwnd, &bb);
        DeleteObject(region);
    }

    // 置顶。
    SetWindowPos(hwnd, options.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 屏幕录制/截图排除：桌宠出现在直播画面里通常不是用户想要的。
    // WDA_EXCLUDEFROMCAPTURE 需要 Win10 2004+，旧版本退化为 WDA_MONITOR（窗口显示为黑块）。
    if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
    }
#elif defined(__APPLE__)
    NSWindow* ns = reinterpret_cast<NSWindow*>(glfwGetCocoaWindow(window_));
    if (!ns) {
        platformNote_ = "无法获取 NSWindow，透明与置顶功能不可用";
        return;
    }
    [ns setOpaque:options.transparent ? NO : YES];
    [ns setBackgroundColor:options.transparent ? [NSColor clearColor] : [NSColor blackColor]];
    [ns setHasShadow:NO];
    [ns setLevel:options.alwaysOnTop ? NSFloatingWindowLevel : NSNormalWindowLevel];
    // macOS 的「独占全屏」应用运行在独立的 Space 里，
    // 只有同时允许加入所有 Space 且允许作为全屏辅助窗口，桌宠才能覆盖在游戏之上。
    [ns setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                             NSWindowCollectionBehaviorFullScreenAuxiliary |
                             NSWindowCollectionBehaviorStationary];
#else
    Display* display = glfwGetX11Display();
    if (!display) {
        platformNote_ = "未获得 X11 display：GLFW 3.3 只实现了 X11 后端，Wayland 会话需要 XWayland";
        return;
    }
    XWindow xwin = glfwGetX11Window(window_);
    if (!xwin) {
        platformNote_ = "无法获取 X11 窗口句柄，点击穿透不可用";
        return;
    }

    // 置顶：GLFW_FLOATING 在 X11 上通过 _NET_WM_STATE_ABOVE 实现。
    glfwSetWindowAttrib(window_, GLFW_FLOATING, options.alwaysOnTop ? GLFW_TRUE : GLFW_FALSE);
    X11SetAbove(display, xwin, options.alwaysOnTop);

    // 不进任务栏：GLFW 3.3 没有 GLFW_SKIP_TASKBAR，这里用 EWMH 的窗口类型实现。
    if (options.skipTaskbar) {
        X11SetWindowTypeUtility(display, xwin);
    }

    // 点击穿透：XShape 输入区域。
    nativeClickThrough_ = X11HasShape(display);
    if (nativeClickThrough_) {
        X11SetInputRegion(display, xwin, lastWidth_, lastHeight_, false);
    } else {
        platformNote_ = "XShape 扩展不可用，点击穿透退化为「仅屏蔽窗口自身事件处理」";
    }

    // Linux/X11 的透明来自 GLFW_TRANSPARENT_FRAMEBUFFER，需要运行中的合成器
    // （compositor）才能看到真正的透明效果，否则窗口为不透明矩形。
    if (options.transparent) {
        platformNote_ += platformNote_.empty() ? "" : "；";
        platformNote_ += "透明背景需要运行中的桌面合成器";
    }
#endif
}

bool Window::InitGL(std::string& error) {
    if (glReady_) return true;
    if (!window_) {
        error = "InitGL 失败：窗口尚未创建";
        return false;
    }

    // 1) 把 GL 上下文绑定到当前线程。GLFW 创建窗口时已经建好了上下文，
    //    但不会自动设为当前，必须先显式 make current，否则后续 GL 调用无上下文。
    glfwMakeContextCurrent(window_);
    // 垂直同步：桌宠动画不需要极高帧率，开启后可避免空转占用 CPU。
    glfwSwapInterval(1);

    // 2) core profile 下必须先打开 glewExperimental，否则部分函数会报「无效操作」。
    glewExperimental = GL_TRUE;
    const GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK) {
        error = std::string("GLEW 初始化失败：") +
                reinterpret_cast<const char*>(glewGetErrorString(glewStatus));
        return false;
    }
    // glewInit 之后常会留下一个 GL_INVALID_ENUM 之类的历史错误，清掉以免后续
    // GL 错误检查（PPET_GL_CHECK）误报。
    while (glGetError() != GL_NO_ERROR) {
    }

    glReady_ = true;
    return true;
}

void Window::UpdatePlatformStateImpl() {
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    if (!hwnd) return;

    // ---- 全屏独占检测 ----
    // 独占全屏的游戏切换显示模式时会重置 Z 序，把普通置顶窗口压到后面。
    // 判定方法：前台窗口的矩形覆盖了其所在显示器的整个区域。
    bool fullscreen = false;
    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != hwnd) {
        RECT windowRect{};
        if (GetWindowRect(foreground, &windowRect)) {
            HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info)) {
                const RECT& area = info.rcMonitor;
                constexpr int kTolerance = 2;  // 允许 2px 误差（边框/任务栏残留）
                fullscreen = windowRect.left <= area.left + kTolerance &&
                             windowRect.top <= area.top + kTolerance &&
                             windowRect.right >= area.right - kTolerance &&
                             windowRect.bottom >= area.bottom - kTolerance;
            }
        }
    }
    fullscreenHostActive_ = fullscreen;

    if (!alwaysOnTop_) return;

    // ---- 置顶维持 ----
    // 全屏独占时每 250ms 补一次置顶，普通情况下每 2s 补一次：
    // 前者保证游戏切回桌面后窗口立即回到顶层，后者防止其它程序（例如某些
    // 全屏播放器）把窗口压下去，同时不至于过于频繁地干扰系统。
    const double now = NowSeconds();
    const double interval = fullscreen ? 0.25 : 2.0;
    if (now - lastTopmostReassert_ >= interval) {
        lastTopmostReassert_ = now;
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
#elif defined(__APPLE__)
    NSWindow* ns = reinterpret_cast<NSWindow*>(glfwGetCocoaWindow(window_));
    if (!ns) return;

    // macOS 没有 Win32 那种「独占全屏压住窗口」的问题（全屏应用在独立 Space），
    // 但切换 Space / 切换应用时层级可能被重置，这里按固定间隔补一次层级。
    NSWindow* key = [[NSApplication sharedApplication] keyWindow];
    if (key && key != ns) {
        NSRect screenRect = [[key screen] frame];
        NSRect frame = [key frame];
        fullscreenHostActive_ = NSWidth(screenRect) > 0.0 &&
                                NSWidth(frame) >= NSWidth(screenRect) - 2.0 &&
                                NSHeight(frame) >= NSHeight(screenRect) - 2.0;
    }

    if (!alwaysOnTop_) return;
    const double now = NowSeconds();
    if (now - lastTopmostReassert_ >= 1.0) {
        lastTopmostReassert_ = now;
        [ns setLevel:NSFloatingWindowLevel];
    }
#else
    Display* display = glfwGetX11Display();
    if (!display) return;
    XWindow xwin = glfwGetX11Window(window_);
    if (!xwin) return;

    // X11 没有「独占全屏」这一模式，但全屏游戏同样可能把窗口压下去，
    // 因此这里周期性补发 _NET_WM_STATE_ABOVE 维持置顶。
    if (!alwaysOnTop_) return;
    const double now = NowSeconds();
    if (now - lastTopmostReassert_ >= 1.0) {
        lastTopmostReassert_ = now;
        glfwSetWindowAttrib(window_, GLFW_FLOATING, GLFW_TRUE);
        X11SetAbove(display, xwin, true);
    }
#endif
}

// ---------------------------------------------------------------------------
// GLFW 回调（静态成员，保证能访问私有状态）
// ---------------------------------------------------------------------------

void Window::GlfwKeyCallback(GLFWwindow* handle, int key, int scancode, int action,
                              int /*mods*/) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (self && self->onKey) self->onKey(key, scancode, action);
}

void Window::GlfwCursorPosCallback(GLFWwindow* handle, double xpos, double ypos) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!self) return;
    // XShape 不可用时的降级：窗口仍然会收到事件，但不再向上层派发。
    if (self->ignoreMouseEvents_ && !self->nativeClickThrough_) return;
    if (self->onMouseMove) self->onMouseMove(xpos, ypos);
}

void Window::GlfwCursorEnterCallback(GLFWwindow* handle, int entered) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!self) return;
    // GLFW 3.3 只有一个进入/离开回调：entered == 1 进入，entered == 0 离开。
    if (self->ignoreMouseEvents_ && !self->nativeClickThrough_) return;
    if (entered) {
        if (self->onMouseEnter) self->onMouseEnter();
    } else {
        if (self->onMouseLeave) self->onMouseLeave();
    }
}

void Window::GlfwMouseButtonCallback(GLFWwindow* handle, int button, int action,
                                      int /*mods*/) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!self) return;
    if (self->ignoreMouseEvents_ && !self->nativeClickThrough_) return;
    if (self->onMouseButton) self->onMouseButton(button, action);
}

void Window::GlfwScrollCallback(GLFWwindow* handle, double dx, double dy) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!self) return;
    if (self->ignoreMouseEvents_ && !self->nativeClickThrough_) return;
    if (self->onScroll) self->onScroll(dx, dy);
}

void Window::GlfwWindowSizeCallback(GLFWwindow* handle, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!self) return;
    self->lastWidth_ = width;
    self->lastHeight_ = height;
#if !defined(_WIN32) && !defined(__APPLE__)
    // 尺寸变化后需要重建 XShape 输入区域，否则点击穿透范围仍是旧尺寸。
    if (self->ignoreMouseEvents_ && self->nativeClickThrough_) {
        Display* display = glfwGetX11Display();
        XWindow xwin = glfwGetX11Window(handle);
        if (display && xwin) {
            X11SetInputRegion(display, xwin, width, height, true);
        }
    }
#endif
}

void Window::GlfwFramebufferSizeCallback(GLFWwindow* handle, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!self) return;
    if (self->onResize) self->onResize(width, height);
}

void Window::GlfwWindowCloseCallback(GLFWwindow* handle) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (self && self->onClose) self->onClose();
}

}  // namespace ppet
