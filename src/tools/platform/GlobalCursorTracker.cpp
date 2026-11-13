// PPet - C++ 移植版
// 全局鼠标追踪：平台实现。
#include <src/tools/platform/GlobalCursorTracker.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
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
#if defined(__APPLE__)
    // 单个 NSObject 指针即可承载 monitor，无需前向声明 Objective-C 类。
    id monitor = nil;
    id globalMonitor = nil;
#elif !defined(_WIN32)
    Display* display = nullptr;
#endif
    // Windows 不需要任何平台句柄：每帧直接 GetCursorPos 轮询，
    // 不安装系统钩子，也就没有钩子线程与跨线程位置传递的需要。

    /// 收到全局鼠标移动事件时调用（macOS 监听器使用）。
    void OnGlobalCursorMoved(float x, float y) {
        pendingX = x;
        pendingY = y;
        hasPending = true;
    }

    std::atomic<float> pendingX{0.0f};
    std::atomic<float> pendingY{0.0f};
    std::atomic<bool> hasPending{false};
};

namespace {

#if defined(__APPLE__)

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
    // 【不再安装 WH_MOUSE_LL 全局低级鼠标钩子】
    // 低级鼠标钩子的回调运行在**系统输入链**上：每次鼠标移动，Windows 都要
    // 同步等待本进程回调返回。一旦钩子线程被阻塞、或与第三方 overlay 注入层
    // （游戏加加 GamePP / RTSS / MSI Afterburner / 录屏与帧率监控软件）的
    // 钩子互相等待，整个系统的鼠标消息都会被拖慢，表现为「连鼠标一起卡」。
    // 另外 WH_MOUSE_LL 的 lParam 是 MSLLHOOKSTRUCT* 而不是 MSG*，旧实现按
    // MSG 解读 msg->message，实际从未正确投递过位置；Stop() 与回调之间还存在
    // owner 悬垂指针窗口，可能直接触发 0xC0000005。
    //
    // 全屏追踪只需要「光标在哪」，GetCursorPos 就能给出整个虚拟桌面的绝对
    // 坐标：不受点击穿透、窗口边界、其它应用遮挡影响，也不进入系统钩子链。
    // 因此这里只创建 Impl，真正的坐标由 Update() 每帧轮询。
    (void)error;  // Windows 轮询没有可失败的资源申请。

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

#if defined(__APPLE__)
    if (impl_->globalMonitor) {
        [NSEvent removeMonitor:impl_->globalMonitor];
        impl_->globalMonitor = nil;
    }
    impl_->monitor = nil;

#elif !defined(_WIN32)
    // Windows 没有任何需要释放的系统资源（无钩子、无线程、无 Display）。
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

#if defined(_WIN32)
    // 每帧一次 GetCursorPos：整个虚拟桌面的绝对坐标，不进入系统钩子链。
    // 点击穿透、全屏追踪关闭、光标在其它显示器上都照样有效。
    Vec2 osCursor;
    if (QueryCursor(osCursor)) {
        state_.position = osCursor;
        state_.valid = true;
    }
#else
    // 优先消费监听器投递的位置（macOS）。
    if (impl_ && impl_->hasPending.exchange(false)) {
        state_.position = Vec2(impl_->pendingX.load(), impl_->pendingY.load());
        state_.valid = true;
    }

#if !defined(__APPLE__)
    // X11 轮询：即使没有 XRecord 也能拿到全局坐标。
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

bool GlobalCursorTracker::QueryCursor(Vec2& out) {
#if defined(_WIN32)
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    // GetCursorPos 返回的是整个虚拟桌面坐标（多显示器时可为负），与钩子一致。
    out = Vec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
    return true;
#elif defined(__APPLE__)
    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) return false;
    const NSRect frame = [screen frame];
    const NSPoint p = [NSEvent mouseLocation];
    out = Vec2(static_cast<float>(p.x - frame.origin.x),
               static_cast<float>(frame.origin.y - p.y));
    return true;
#else
    // X11：进程级缓存一个 Display，避免每帧 XOpenDisplay 的握手开销。
    // XQueryPointer 对 root 窗口查询的是全局坐标，即使指针在别的窗口、
    // 或本窗口开启了点击穿透也照样有效。
    static std::mutex xMutex;
    static Display* cachedDisplay = nullptr;
    std::lock_guard<std::mutex> lock(xMutex);
    if (!cachedDisplay) cachedDisplay = XOpenDisplay(nullptr);
    if (!cachedDisplay) return false;
    Window root = DefaultRootWindow(cachedDisplay);
    Window rootRet = 0, childRet = 0;
    int rootX = 0, rootY = 0, winX = 0, winY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(cachedDisplay, root, &rootRet, &childRet, &rootX, &rootY, &winX,
                       &winY, &mask)) {
        return false;
    }
    out = Vec2(static_cast<float>(rootX), static_cast<float>(rootY));
    return true;
#endif
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
