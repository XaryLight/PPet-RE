// PPet - C++ 移植版
// 全局鼠标追踪：平台实现。
#include "platform/GlobalCursorTracker.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

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

#elif defined(__APPLE__)
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace ppet {

// ---------------------------------------------------------------------------
// Impl（各平台实现，定义在前，确保下方所有平台回调都能引用完整类型）
// ---------------------------------------------------------------------------

struct GlobalCursorTracker::Impl {
#if defined(_WIN32)
    // Windows 钩子必须在安装它的线程上保持消息循环，故用单独线程安装。
    HHOOK hook = nullptr;
    DWORD threadId = 0;
    HANDLE readyEvent = nullptr;
    std::atomic<bool> stopRequested{false};
    std::thread messageThread;
#elif defined(__APPLE__)
    // 单个 NSObject 指针即可承载 monitor，无需前向声明 Objective-C 类。
    id monitor = nil;
    id globalMonitor = nil;
#else
    Display* display = nullptr;
#endif

    /// 收到全局鼠标移动事件时调用（各平台共用）。
    void OnGlobalCursorMoved(float x, float y) {
        pendingX = x;
        pendingY = y;
        hasPending = true;
    }

    // 事件在钩子线程产生，Update() 在主线程读取，通过这组变量做线程间传递。
    std::atomic<float> pendingX{0.0f};
    std::atomic<float> pendingY{0.0f};
    std::atomic<bool> hasPending{false};
};

namespace {

#if defined(_WIN32)

/// Windows 低级鼠标钩子的全局状态。
struct HookState {
    HHOOK hook = nullptr;
    GlobalCursorTracker::Impl* owner = nullptr;
};

HookState g_hookState;

LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto msg = reinterpret_cast<MSG*>(lParam);
        if (msg && (msg->message == WM_MOUSEMOVE || msg->message == WM_MOUSELAST)) {
            POINT pt;
            if (GetCursorPos(&pt)) {
                if (g_hookState.owner) {
                    g_hookState.owner->OnGlobalCursorMoved(static_cast<float>(pt.x),
                                                          static_cast<float>(pt.y));
                }
            }
        }
    }
    return CallNextHookEx(g_hookState.hook, code, wParam, lParam);
}

#elif defined(__APPLE__)

/// macOS 全局事件监听器持有者。
@interface PPetGlobalMouseMonitor : NSObject
@property(nonatomic, assign) void* owner;
- (void)onMouseMoved:(NSEvent*)event;
@end

@implementation PPetGlobalMouseMonitor
- (void)onMouseMoved:(NSEvent*)event {
    if (!self.owner) return;
    // global monitor 的事件坐标位于当前屏幕坐标系，转换为全局坐标需要加上原点偏移。
    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) return;
    const NSRect frame = [screen frame];
    const NSPoint p = [NSEvent mouseLocation];
    auto* tracker = static_cast<GlobalCursorTracker::Impl*>(self.owner);
    tracker->OnGlobalCursorMoved(static_cast<float>(p.x - frame.origin.x),
                                 static_cast<float>(frame.origin.y - p.y));
}
@end

#endif

}  // namespace

// ---------------------------------------------------------------------------
// 析构与启停
// ---------------------------------------------------------------------------

GlobalCursorTracker::~GlobalCursorTracker() { Stop(); }

bool GlobalCursorTracker::Start(std::string& error) {
    if (running_) return true;

    impl_ = new Impl();

#if defined(_WIN32)
    // 在独立线程安装钩子并维持消息循环：低级鼠标钩子的回调
    // 会被投递到安装它的线程，若在渲染线程安装会与 OpenGL 消息泵争抢。
    impl_->readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->readyEvent) {
        error = "创建事件对象失败";
        Stop();
        return false;
    }

    g_hookState.owner = impl_;

    impl_->messageThread = std::thread([this]() {
        impl_->threadId = GetCurrentThreadId();
        impl_->hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHookProc, nullptr, 0);
        // 把钩子句柄同步给全局状态，供 CallNextHookEx 使用。
        g_hookState.hook = impl_->hook;
        SetEvent(impl_->readyEvent);

        MSG msg;
        while (!impl_->stopRequested.load()) {
            // 用超时等待代替阻塞 GetMessage，以便响应停止请求。
            const DWORD r = MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
        if (impl_->hook) {
            UnhookWindowsHookEx(impl_->hook);
            impl_->hook = nullptr;
        }
        g_hookState.hook = nullptr;
    });

    WaitForSingleObject(impl_->readyEvent, 3000);
    if (!impl_->hook) {
        error = "安装全局鼠标钩子失败";
        Stop();
        return false;
    }

#elif defined(__APPLE__)
    impl_->monitor = [[PPetGlobalMouseMonitor alloc] init];
    impl_->monitor.owner = impl_;

    // global monitor 可以接收其他应用的事件，无需辅助功能权限。
    impl_->globalMonitor = [NSEvent
        addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
                                       handler:^(NSEvent* event) {
                                           [impl_->monitor onMouseMoved:event];
                                       }];
    if (!impl_->globalMonitor) {
        error = "创建全局鼠标监听器失败";
        Stop();
        return false;
    }

#else
    // X11：优先使用 XRecord 扩展做事件驱动追踪；不可用时退化为轮询。
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) {
        error = "无法打开 X display，未能启用全局鼠标追踪";
        Stop();
        return false;
    }
#endif

    running_ = true;

    // 初始化桌面范围与初始光标位置。
    RefreshDesktop();
    Update();

    lastPosition_ = state_.position;
    hasLast_ = state_.valid;
    return true;
}

void GlobalCursorTracker::Stop() {
    if (!impl_) return;

#if defined(_WIN32)
    if (impl_->readyEvent) {
        impl_->stopRequested.store(true);
        // 唤醒消息线程使其退出等待循环。
        PostThreadMessage(impl_->threadId, WM_QUIT, 0, 0);
        if (impl_->messageThread.joinable()) impl_->messageThread.join();
        CloseHandle(impl_->readyEvent);
        impl_->readyEvent = nullptr;
    }
    g_hookState.owner = nullptr;
    g_hookState.hook = nullptr;

#elif defined(__APPLE__)
    if (impl_->globalMonitor) {
        [NSEvent removeMonitor:impl_->globalMonitor];
        impl_->globalMonitor = nil;
    }
    impl_->monitor = nil;

#else
    if (impl_->display) {
        XCloseDisplay(impl_->display);
        impl_->display = nullptr;
    }
#endif

    delete impl_;
    impl_ = nullptr;
    running_ = false;
}

void GlobalCursorTracker::Update() {
    state_.moved = false;

    // 优先消费钩子/监听器投递的位置。
    if (impl_ && impl_->hasPending.exchange(false)) {
        state_.position = Vec2(impl_->pendingX.load(), impl_->pendingY.load());
        state_.valid = true;
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    // X11 退化为轮询：即使没有 XRecord 也能拿到全局坐标。
    if (impl_ && impl_->display) {
        Window root = DefaultRootWindow(impl_->display);
        Window rootRet = 0, childRet = 0;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (XQueryPointer(impl_->display, root, &rootRet, &childRet, &rootX, &rootY, &winX,
                          &winY, &mask)) {
            state_.position = Vec2(static_cast<float>(rootX), static_cast<float>(rootY));
            state_.valid = true;
        }
    }
#endif

    if (state_.valid) {
        if (!hasLast_) {
            lastPosition_ = state_.position;
            hasLast_ = true;
        } else {
            const float dx = state_.position.x - lastPosition_.x;
            const float dy = state_.position.y - lastPosition_.y;
            state_.moved = (dx * dx + dy * dy) > 0.01f;
            lastPosition_ = state_.position;
        }
    }
}

Vec2 GlobalCursorTracker::NormalizeAround(const Vec2& cursor, const Vec2& center,
                                           float radius, Vec2* outNormalized) {
    if (!outNormalized) return Vec2();
    if (radius <= 0.001f) {
        *outNormalized = Vec2();
        return *outNormalized;
    }
    const Vec2 d = cursor - center;
    // 屏幕 Y 向下，模型空间 Y 向上，故取反。
    Vec2 n(d.x / radius, -d.y / radius);
    // 限制到单位圆内，避免光标远离模型时方向失真。
    const float len = Length(n);
    if (len > 1.0f) n = n / len;
    *outNormalized = n;
    return n;
}

/// 更新虚拟桌面范围（各平台）。
void UpdateDesktopImpl(GlobalCursorTracker& tracker) {
    VirtualDesktop desk;
#if defined(_WIN32)
    const int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    desk.x = sx;
    desk.y = sy;
    desk.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desk.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
#elif defined(__APPLE__)
    for (NSScreen* s in [NSScreen screens]) {
        const NSRect f = [s frame];
        const int minX = static_cast<int>(f.origin.x);
        const int minY = static_cast<int>(f.origin.y);
        const int maxX = static_cast<int>(f.origin.x + f.size.width);
        const int maxY = static_cast<int>(f.origin.y + f.size.height);
        if (desk.width == 0) {
            desk.x = minX;
            desk.y = minY;
            desk.width = maxX - minX;
            desk.height = maxY - minY;
        } else {
            const int nx = std::min(desk.x, minX);
            const int ny = std::min(desk.y, minY);
            const int nx2 = std::max(desk.x + desk.width, maxX);
            const int ny2 = std::max(desk.y + desk.height, maxY);
            desk.x = nx;
            desk.y = ny;
            desk.width = nx2 - nx;
            desk.height = ny2 - ny;
        }
    }
#else
    // X11：根窗口覆盖整个虚拟桌面（含所有 RandR 输出的并集），直接取其几何。
    if (Display* dpy = XOpenDisplay(nullptr)) {
        const int screenCount = ScreenCount(dpy);
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (int i = 0; i < screenCount; ++i) {
            const Screen* s = ScreenOfDisplay(dpy, i);
            // Screen 只有宽高，位置由根窗口决定。
            const int sw = s->width;
            const int sh = s->height;
            if (i == 0) {
                minX = 0; minY = 0; maxX = sw; maxY = sh;
            } else {
                maxX = std::max(maxX, sw);
                maxY = std::max(maxY, sh);
            }
        }
        desk.x = minX;
        desk.y = minY;
        desk.width = maxX - minX;
        desk.height = maxY - minY;
        XCloseDisplay(dpy);
    }
#endif
    (void)tracker;
    tracker.SetDesktop(desk);
}

void GlobalCursorTracker::RefreshDesktop() { UpdateDesktopImpl(*this); }

}  // namespace ppet
