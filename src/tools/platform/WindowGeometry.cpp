// PPet - C++ 移植版
// 窗口几何查询：平台实现。
#include <src/tools/platform/WindowGeometry.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <dwmapi.h>

#elif defined(__APPLE__)
#include <CoreGraphics/CGWindow.h>

#else
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

namespace ppet {
namespace {

#if defined(_WIN32)

/// 用 DwmGetWindowAttribute 取窗口真实可见边框（含 DWM 扩展边框），失败回退
/// GetWindowRect。返回虚拟桌面坐标（可为负，多显示器）。
WindowRect RectForHwnd(HWND hwnd) {
    WindowRect out;
    if (!hwnd || !IsWindow(hwnd)) return out;

    RECT r{};
    // DWMWA_EXTENDED_FRAME_BOUNDS 是「含 DWM 扩展边框的可见矩形」，比
    // GetWindowRect 更贴近用户看到的窗口边缘（GetWindowRect 在 DWM 下会包含
    // 隐藏的阴影区域）。失败（老系统 / 无 DWM）时回退 GetWindowRect。
    const HRESULT hr =
        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r));
    if (FAILED(hr)) {
        if (!GetWindowRect(hwnd, &r)) return out;
    }
    out.x = static_cast<float>(r.left);
    out.y = static_cast<float>(r.top);
    out.width = static_cast<float>(r.right - r.left);
    out.height = static_cast<float>(r.bottom - r.top);
    out.valid = out.width > 0.0f && out.height > 0.0f;
    return out;
}

#elif defined(__APPLE__)

/// 把 CGWindow 的 bounds（CGRect，全局坐标、y 向下）转成 WindowRect。
/// CGWindow bounds 使用的坐标系原点在主屏左上角、y 向下，与 Windows/X11 约定
/// 一致，直接透传即可，无需 y 翻转。
WindowRect RectForCgBounds(const CGRect& b) {
    WindowRect out;
    out.x = static_cast<float>(b.origin.x);
    out.y = static_cast<float>(b.origin.y);
    out.width = static_cast<float>(b.size.width);
    out.height = static_cast<float>(b.size.height);
    out.valid = out.width > 0.0f && out.height > 0.0f;
    return out;
}

/// 遍历屏幕上的窗口，返回第一个满足 predicate 的窗口矩形。
/// CGWindowListCopyWindowInfo 返回的数组顺序是「从前到后」（第一个最靠前），
/// 因此按序遍历即「最前优先」。kCGWindowListExcludeDesktopElements 排除桌面/
/// Dock/菜单栏等系统窗口（它们 layer 不为 0）。
template <typename Pred>
WindowRect FindWindow(Pred pred) {
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!list) return WindowRect{};

    WindowRect found;
    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; ++i) {
        const void* item = CFArrayGetValueAtIndex(list, i);
        if (!item) continue;
        // 用 CFDictionary 桥接；CGWindow 返回的每一项都是 CFDictionary。
        CFDictionaryRef dict = static_cast<CFDictionaryRef>(item);
        // 只考虑普通窗口（layer == 0），跳过菜单栏/状态栏/通知中心等浮层。
        CFNumberRef layerRef = static_cast<CFNumberRef>(
            CFDictionaryGetValue(dict, kCGWindowLayer));
        int layer = 0;
        if (layerRef) CFNumberGetValue(layerRef, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        CGRect bounds{};
        CFDictionaryRef boundsRef = static_cast<CFDictionaryRef>(
            CFDictionaryGetValue(dict, kCGWindowBounds));
        if (!boundsRef) continue;
        CGRectMakeWithDictionaryRepresentation(boundsRef, &bounds);
        if (pred(bounds)) {
            found = RectForCgBounds(bounds);
            break;
        }
    }
    CFRelease(list);
    return found;
}

#else

/// 取 X11 窗口在根窗口坐标系下的绝对边框（左上角 + 尺寸）。
WindowRect RectForXWindow(Display* dpy, Window win) {
    WindowRect out;
    if (!dpy || win == 0) return out;

    const Window root = DefaultRootWindow(dpy);
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(dpy, win, &attrs)) return out;

    // XGetWindowAttributes 的 x/y 是相对父窗口的，多显示器 / WM reparent 下
    // 可能不是根坐标；XTranslateCoordinates 把窗口原点换算到根坐标。
    int absX = 0, absY = 0;
    Window child = 0;
    if (!XTranslateCoordinates(dpy, win, root, 0, 0, &absX, &absY, &child)) {
        absX = attrs.x;
        absY = attrs.y;
    }
    out.x = static_cast<float>(absX);
    out.y = static_cast<float>(absY);
    out.width = static_cast<float>(attrs.width);
    out.height = static_cast<float>(attrs.height);
    out.valid = out.width > 0.0f && out.height > 0.0f;
    return out;
}

/// 读取根窗口上的 _NET_ACTIVE_WINDOW（EWMH），得到当前活跃窗口 id。
Window ActiveWindowId(Display* dpy) {
    const Window root = DefaultRootWindow(dpy);
    const Atom netActive = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (netActive == None) return 0;

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* prop = nullptr;
    const int status = XGetWindowProperty(dpy, root, netActive, 0, 1, False, XA_WINDOW,
                                          &actualType, &actualFormat, &nitems,
                                          &bytesAfter, &prop);
    Window active = 0;
    if (status == Success && prop && actualType == XA_WINDOW && nitems > 0) {
        active = *reinterpret_cast<Window*>(prop);
    }
    if (prop) XFree(prop);
    return active;
}

/// 从光标下的窗口向上找顶层窗口（父为 root 者）。找不到就返回入参。
Window TopLevelAncestor(Display* dpy, Window win) {
    const Window root = DefaultRootWindow(dpy);
    Window cur = win;
    // 最多向上 16 层，防止异常窗口树造成死循环。
    for (int i = 0; i < 16 && cur != 0 && cur != root; ++i) {
        Window r = 0, parent = 0, *children = nullptr;
        unsigned int n = 0;
        if (!XQueryTree(dpy, cur, &r, &parent, &children, &n)) break;
        if (children) XFree(children);
        if (parent == 0 || parent == root) return cur;
        cur = parent;
    }
    return win;
}

#endif

}  // namespace

WindowRect WindowGeometry::ForegroundWindow() {
#if defined(_WIN32)
    // 前台窗口：用户当前交互的窗口。桌宠窗口点击穿透时不抢焦点，天然被排除。
    return RectForHwnd(GetForegroundWindow());

#elif defined(__APPLE__)
    // 最前的普通窗口（layer 0）即用户当前关注的窗口。
    return FindWindow([](const CGRect&) { return true; });

#else
    WindowRect out;
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return out;  // Wayland / 无 DISPLAY：不可用，坐姿自动禁用
    const Window active = ActiveWindowId(dpy);
    if (active != 0) out = RectForXWindow(dpy, active);
    XCloseDisplay(dpy);
    return out;
#endif
}

WindowRect WindowGeometry::WindowUnderCursor(float cursorX, float cursorY) {
#if defined(_WIN32)
    const POINT pt{static_cast<LONG>(cursorX), static_cast<LONG>(cursorY)};
    return RectForHwnd(WindowFromPoint(pt));

#elif defined(__APPLE__)
    const CGFloat cx = static_cast<CGFloat>(cursorX);
    const CGFloat cy = static_cast<CGFloat>(cursorY);
    return FindWindow([&](const CGRect& b) {
        return cx >= b.origin.x && cx <= b.origin.x + b.size.width &&
               cy >= b.origin.y && cy <= b.origin.y + b.size.height;
    });

#else
    // X11 下「光标下的窗口」由 XQueryPointer 的 childRet 直接给出（自带光标
    // 位置），传入的 cursorX/cursorY 只是语义冗余（与 QueryCursor 结果一致）。
    (void)cursorX;
    (void)cursorY;
    WindowRect out;
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return out;  // Wayland / 无 DISPLAY
    const Window root = DefaultRootWindow(dpy);
    Window rootRet = 0, childRet = 0;
    int rootX = 0, rootY = 0, winX = 0, winY = 0;
    unsigned int mask = 0;
    if (XQueryPointer(dpy, root, &rootRet, &childRet, &rootX, &rootY, &winX, &winY,
                      &mask) &&
        childRet != 0) {
        out = RectForXWindow(dpy, TopLevelAncestor(dpy, childRet));
    }
    XCloseDisplay(dpy);
    return out;
#endif
}

}  // namespace ppet
