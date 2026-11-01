// PPet - C++ 移植版
// 极简「系统托盘宿主」模拟器 —— 仅用于在测试环境中验证 Linux 托盘实现。
//
// 真实桌面环境由面板（GNOME Shell / KDE Plasma 等）提供 _NET_SYSTEM_TRAY 协议，
// 无人值守的测试环境里没有面板，托盘代码会走「无托盘降级模式」，
// 因此本程序模拟一个最小可用的托盘宿主：
//   1. 创建一个窗口并持有 _NET_SYSTEM_TRAY_S0 选择；
//   2. 收到 _NET_SYSTEM_TRAY_DOCK 后，在客户端窗口上设置 _NET_SYSTEM_TRAY_OPCODE 属性；
//   3. 回发 opcode = 0 (CREATE)，通知客户端显示图标。
//
// 用法：fake_tray &   然后运行 ppet_platform_smoke
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>

int main() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::fprintf(stderr, "fake_tray: 无法打开 X display\n");
        return 1;
    }
    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);

    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    const Window host = XCreateWindow(display, root, 0, 0, 64, 64, 0, CopyFromParent, InputOutput,
                                      CopyFromParent, CWOverrideRedirect, &attributes);
    XSelectInput(display, host, StructureNotifyMask);
    XMapWindow(display, host);

    const Atom selection = XInternAtom(display, "_NET_SYSTEM_TRAY_S0", False);
    XSetSelectionOwner(display, selection, host, CurrentTime);

    const Atom dockAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_DOCK", False);
    const Atom opcodeAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);

    std::printf("fake_tray: 就绪（宿主窗口 %lu）\n", static_cast<unsigned long>(host));
    std::fflush(stdout);

    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type != ClientMessage || event.xclient.message_type != dockAtom) continue;

        const Window client = static_cast<Window>(event.xclient.data.l[1]);
        std::printf("fake_tray: 收到停靠请求，客户端窗口 %lu\n",
                    static_cast<unsigned long>(client));
        std::fflush(stdout);

        // 协议规定：宿主在客户端窗口上设置 _NET_SYSTEM_TRAY_OPCODE 属性。
        unsigned long opcode = 0;
        XChangeProperty(display, client, opcodeAtom, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&opcode), 1);

        // 回发 CREATE，通知客户端可以显示图标了。
        XEvent create{};
        create.xclient.type = ClientMessage;
        create.xclient.window = client;
        create.xclient.message_type = opcodeAtom;
        create.xclient.format = 32;
        create.xclient.data.l[0] = CurrentTime;
        create.xclient.data.l[1] = 0;  // 0 = CREATE
        create.xclient.data.l[2] = 0;
        create.xclient.data.l[3] = 0;
        XSendEvent(display, client, False, NoEventMask, &create);
        XFlush(display);
    }
    return 0;
}
