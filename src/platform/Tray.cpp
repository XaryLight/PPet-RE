// PPet - C++ 移植版
// 系统托盘层实现。
//
// 三个平台各自实现，互不共享代码，只共享「菜单扁平索引」这一约定：
//   菜单树按「父项在前、子项紧随其后」的顺序展开成扁平索引，
//   平台层把该索引编码进命令 ID（Windows）或 tag（macOS），
//   用户选中后回传到 Tray::NotifyItemSelected(index)。
#include "platform/Tray.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// 显式锁定 Windows 目标版本，确保 MF_RADIOCHECK 等较新菜单标志可用。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>

// 个别交叉编译工具链（如部分 MinGW/llvm-mingw 配置）的 winuser.h 未暴露
// MF_RADIOCHECK，这里做兜底定义（0x0200 为官方取值）。
#ifndef MF_RADIOCHECK
#define MF_RADIOCHECK 0x0200
#endif

#elif defined(__APPLE__)
// Apple 平台需要以 Objective-C++ 编译（CMake 中通过 LANGUAGE OBJCXX 指定）。
#import <AppKit/AppKit.h>

#else
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#if defined(PPET_HAS_XFT)
// Xft 用于绘制托盘菜单文字（支持中文；核心 X 字体无法绘制 UTF-8）。
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#endif
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#if !defined(PPET_HAS_XFT)
// 核心 X 字体（XLoadQueryFont / XFontStruct / XTextWidth）的声明位于 X11/Xlib.h，
// 这里作为 Xft 不可用时的降级方案。
#endif
#endif

#if !defined(__APPLE__)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
// stb_image 里有一批仅在 HDR 路径使用的静态函数，单独包含时会触发
// -Wunused-function 告警，这里局部关闭，避免污染「零警告」的编译要求。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb/stb_image.h"
#pragma GCC diagnostic pop
#include <fstream>
#include <iterator>
#endif

namespace ppet {
namespace {

/// 展开菜单树：生成扁平列表、顶层项索引与每个父项的子项数量。
///
/// 扁平顺序为「父项在前、子项紧随其后递归展开」，与 Tray::NotifyItemSelected
/// 使用的索引完全一致，因此平台层可以用扁平索引 O(1) 定位任意菜单项。
///
/// @param tree           菜单树
/// @param flat          输出：扁平后的菜单项（subItems 已清空）
/// @param topLevelIndices 输出：顶层菜单项在扁平列表中的索引
/// @param childCounts   输出：扁平列表每一项的子菜单项数量（非子菜单项为 0）
void FlattenMenuTree(const std::vector<TrayMenuItem>& tree, std::vector<TrayMenuItem>* flat,
                     std::vector<int>* topLevelIndices, std::vector<int>* childCounts) {
    flat->clear();
    topLevelIndices->clear();
    childCounts->clear();
    std::function<void(const std::vector<TrayMenuItem>&, bool)> walk =
        [&](const std::vector<TrayMenuItem>& list, bool isTopLevel) {
            for (const TrayMenuItem& item : list) {
                const int index = static_cast<int>(flat->size());
                TrayMenuItem copy = item;
                copy.subItems.clear();
                flat->push_back(std::move(copy));
                childCounts->push_back(item.type == TrayMenuItem::Type::Submenu
                                           ? static_cast<int>(item.subItems.size())
                                           : 0);
                if (isTopLevel) topLevelIndices->push_back(index);
                if (item.type == TrayMenuItem::Type::Submenu) walk(item.subItems, false);
            }
        };
    walk(tree, true);
}

#if defined(_WIN32) || defined(__APPLE__)
/// 计算顶层菜单项的扁平索引（子菜单项的索引 = 父索引 + 1 + 位置）。
std::vector<int> ComputeTopLevelIndices(const std::vector<TrayMenuItem>& items) {
    std::vector<int> topLevel;
    int counter = 0;
    std::function<void(const std::vector<TrayMenuItem>&, bool)> walk =
        [&](const std::vector<TrayMenuItem>& list, bool isTopLevel) {
            for (const TrayMenuItem& item : list) {
                const int index = counter++;
                if (isTopLevel) topLevel.push_back(index);
                if (item.type == TrayMenuItem::Type::Submenu) walk(item.subItems, false);
            }
        };
    walk(items, true);
    return topLevel;
}

/// 子菜单项的扁平索引：父索引 + 1 + 位置。
std::vector<int> ComputeSubmenuIndices(int parentFlatIndex, std::size_t count) {
    std::vector<int> indices;
    indices.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        indices.push_back(parentFlatIndex + 1 + static_cast<int>(i));
    }
    return indices;
}
#endif

#if !defined(__APPLE__)

/// 读取 PNG 文件并解码为 RGBA 像素（Windows 用于构造 HICON，Linux 用于绘制托盘图标）。
bool LoadPngFile(const std::string& path, std::vector<unsigned char>* outRgba, int* outWidth,
                 int* outHeight) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<unsigned char> data(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()),
                                                  &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) return false;
    outRgba->assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    *outWidth = width;
    *outHeight = height;
    return true;
}

#endif

#if defined(_WIN32)

/// UTF-8 → UTF-16（Win32 的宽字符 API 使用 UTF-16）。
std::wstring ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0],
                        size);
    return result;
}

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr int kCommandBase = 1000;
const wchar_t* const kTrayClassName = L"PPetTrayWindow";

/// Win32 托盘的进程级状态。
///
/// 之所以放在文件作用域而不是 Tray::Impl 里：窗口过程是 C 风格回调，
/// 无法访问 Tray 的私有嵌套类型。这里通过 std::function 把事件转交给 Impl。
struct Win32TrayState {
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid{};
    bool iconAdded = false;
    std::function<void()> onIconEvent;       ///< 图标被点击（弹出菜单）
    std::function<void()> onTaskbarCreated;  ///< 资源管理器重启后重新添加图标
};

Win32TrayState* g_win32State = nullptr;

// 「任务栏已重建」并非 Windows 预定义消息，必须向系统注册（消息值为系统级、
// 跨进程一致），因此无法作为编译期常量出现在 switch 的 case 标签里，
// 只能在窗口过程里用 if 比较。
UINT g_wmTaskbarCreated = 0;

LRESULT CALLBACK Win32TrayWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Win32TrayState* state = g_win32State;
    if (g_wmTaskbarCreated != 0 && message == g_wmTaskbarCreated) {
        // 资源管理器（explorer.exe）重启后会丢失托盘图标，需要重新添加。
        if (state && state->onTaskbarCreated) state->onTaskbarCreated();
        return 0;
    }
    switch (message) {
        case kTrayCallbackMessage: {
            if (state && state->onIconEvent) {
                const UINT event = LOWORD(lparam);
                // 右键弹出菜单（与 Electron 的 setContextMenu 行为一致）；
                // 左键双击也弹出，方便没有其它交互入口时使用。
                if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU ||
                    event == WM_LBUTTONDBLCLK) {
                    state->onIconEvent();
                }
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/// 从 RGBA 像素构造 HICON（32bpp BGRA + 1bpp 掩码）。
HICON CreateIconFromRgba(const unsigned char* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return nullptr;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    if (!dc) {
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    BITMAPINFO colorInfo{};
    colorInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    colorInfo.bmiHeader.biWidth = width;
    colorInfo.bmiHeader.biHeight = -height;  // 负值表示自上而下
    colorInfo.bmiHeader.biPlanes = 1;
    colorInfo.bmiHeader.biBitCount = 32;
    colorInfo.bmiHeader.biCompression = BI_RGB;

    void* colorBits = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(dc, &colorInfo, DIB_RGB_COLORS, &colorBits, nullptr, 0);

    BITMAPINFO maskInfo{};
    maskInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskInfo.bmiHeader.biWidth = width;
    maskInfo.bmiHeader.biHeight = -height;
    maskInfo.bmiHeader.biPlanes = 1;
    maskInfo.bmiHeader.biBitCount = 1;
    maskInfo.bmiHeader.biCompression = BI_RGB;

    void* maskBits = nullptr;
    HBITMAP maskBitmap = CreateDIBSection(dc, &maskInfo, DIB_RGB_COLORS, &maskBits, nullptr, 0);

    if (!colorBitmap || !maskBitmap || !colorBits || !maskBits) {
        if (colorBitmap) DeleteObject(colorBitmap);
        if (maskBitmap) DeleteObject(maskBitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    auto* dst = static_cast<unsigned char*>(colorBits);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char* src = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
            unsigned char* out = dst + (static_cast<std::size_t>(y) * width + x) * 4;
            out[0] = src[2];  // B
            out[1] = src[1];  // G
            out[2] = src[0];  // R
            out[3] = src[3];  // A
        }
    }

    // 单色掩码：alpha 超过阈值视为不透明（0 = 不透明，1 = 透明）。
    auto* mask = static_cast<unsigned char*>(maskBits);
    const int stride = ((width + 31) / 32) * 4;
    std::memset(mask, 0, static_cast<std::size_t>(stride) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char alpha = rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
            if (alpha <= 128) mask[y * stride + x / 8] |= (0x80u >> (x % 8));
        }
    }

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

#elif defined(__APPLE__)

/// UTF-8 → NSString。
NSString* ToNSString(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()];
}

/// ARC 与手动引用计数下的释放写法兼容层。
#if defined(__has_feature)
#if __has_feature(objc_arc)
#define PPET_RELEASE(object) do { } while (0)
#else
#define PPET_RELEASE(object) [(object) release]
#endif
#else
#define PPET_RELEASE(object) [(object) release]
#endif

/// 托盘菜单动作的接收者。
@interface PPetTrayController : NSObject
@property(nonatomic, assign) void* owner;   ///< 指向 Tray 实例
- (void)menuAction:(id)sender;
@end

@implementation PPetTrayController
- (void)menuAction:(id)sender {
    Tray* tray = static_cast<Tray*>(self.owner);
    if (!tray) return;
    NSMenuItem* menuItem = [sender isKindOfClass:[NSMenuItem class]] ? (NSMenuItem*)sender : nil;
    if (!menuItem) return;
    // tag 即菜单项的扁平索引。
    tray->NotifyItemSelected(static_cast<int>([menuItem tag]));
}
@end

#endif

}  // namespace

// ---------------------------------------------------------------------------
// Tray::Impl（各平台实现）
// ---------------------------------------------------------------------------

#if defined(_WIN32)

struct Tray::Impl {
    Win32TrayState state{};
    HMENU menu = nullptr;
    std::vector<TrayMenuItem> items;
    std::vector<int> topLevelIndices;
    std::string iconPath;
    std::string tooltip;

    /// 递归构建 Win32 菜单。indices 给出每一项对应的扁平索引。
    HMENU BuildMenu(HMENU parent, const std::vector<TrayMenuItem>& list,
                    const std::vector<int>& indices) {
        HMENU result = parent ? parent : CreatePopupMenu();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const TrayMenuItem& item = list[i];
            const int flatIndex = i < indices.size() ? indices[i] : static_cast<int>(i);

            if (item.type == TrayMenuItem::Type::Separator) {
                AppendMenuW(result, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            if (item.type == TrayMenuItem::Type::Submenu) {
                HMENU sub = BuildMenu(nullptr, item.subItems,
                                      ComputeSubmenuIndices(flatIndex, item.subItems.size()));
                std::wstring label = ToWide(item.label);
                AppendMenuW(result, MF_POPUP | (item.enabled ? 0u : MF_GRAYED),
                           reinterpret_cast<UINT_PTR>(sub), label.c_str());
                continue;
            }

            UINT flags = MF_STRING;
            if (item.type == TrayMenuItem::Type::Checkbox ||
                item.type == TrayMenuItem::Type::Toggle) {
                if (item.checked) flags |= MF_CHECKED;
            } else if (item.type == TrayMenuItem::Type::Radio) {
                flags |= MF_RADIOCHECK;
                if (item.checked) flags |= MF_CHECKED;
            }
            if (!item.enabled) flags |= MF_GRAYED;

            std::wstring label = ToWide(item.label);
            if (!item.accelerator.empty()) {
                // Win32 用制表符分隔快捷键，显示在菜单右侧。
                label += L"\t";
                label += ToWide(item.accelerator);
            }
            AppendMenuW(result, flags, static_cast<UINT_PTR>(kCommandBase + flatIndex),
                        label.c_str());
        }
        return result;
    }

    void RebuildMenu() {
        if (menu) {
            DestroyMenu(menu);
            menu = nullptr;
        }
        topLevelIndices = ComputeTopLevelIndices(items);
        menu = BuildMenu(nullptr, items, topLevelIndices);
    }

    void ShowMenu() {
        if (!menu) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        // 弹出菜单前必须让窗口获得前台焦点，否则菜单在失去焦点后会立刻消失。
        SetForegroundWindow(state.hwnd);
        const UINT command = TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0,
            state.hwnd, nullptr);
        // 标准做法：弹出结束后投递一个空消息，避免菜单异常残留。
        PostMessage(state.hwnd, WM_NULL, 0, 0);
        if (command == 0) return;
        const int index = static_cast<int>(command) - kCommandBase;
        if (owner) owner->NotifyItemSelected(index);
    }

    void ApplyTooltip() {
        if (!state.iconAdded) return;
        state.nid.uFlags = NIF_TIP;
        const std::wstring text = ToWide(tooltip);
        wcsncpy(state.nid.szTip, text.c_str(), 127);
        state.nid.szTip[127] = L'\0';
        Shell_NotifyIconW(NIM_MODIFY, &state.nid);
    }

    void ApplyIcon() {
        if (!state.iconAdded) return;
        HICON icon = nullptr;
        if (!iconPath.empty()) {
            // 优先按 PNG 解析（与原项目使用的 tray.png 一致）；
            // 其它格式退回到系统的 LoadImage。
            std::vector<unsigned char> pixels;
            int width = 0;
            int height = 0;
            if (LoadPngFile(iconPath, &pixels, &width, &height)) {
                icon = CreateIconFromRgba(pixels.data(), width, height);
            } else {
                const std::wstring wide = ToWide(iconPath);
                icon = static_cast<HICON>(
                    LoadImageW(nullptr, wide.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
            }
        }
        if (icon) {
            state.nid.uFlags = NIF_ICON;
            state.nid.hIcon = icon;
            Shell_NotifyIconW(NIM_MODIFY, &state.nid);
            // Shell_NotifyIcon 复制了图标句柄，本地这份可以释放。
            DestroyIcon(icon);
        }
    }

    Tray* owner = nullptr;
};

#elif defined(__APPLE__)

struct Tray::Impl {
    NSStatusItem* statusItem = nil;
    PPetTrayController* controller = nil;
    std::vector<TrayMenuItem> items;
    std::vector<int> topLevelIndices;
    std::string iconPath;
    std::string tooltip;

    /// 递归构建 NSMenu。indices 给出每一项对应的扁平索引。
    NSMenu* BuildMenu(const std::vector<TrayMenuItem>& list, const std::vector<int>& indices) {
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"PPet"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            const TrayMenuItem& item = list[i];
            const int flatIndex = i < indices.size() ? indices[i] : static_cast<int>(i);

            if (item.type == TrayMenuItem::Type::Separator) {
                [menu addItem:[NSMenuItem separatorItem]];
                continue;
            }

            NSMenuItem* menuItem = [[NSMenuItem alloc] initWithTitle:ToNSString(item.label)
                                                             action:@selector(menuAction:)
                                                      keyEquivalent:@""];
            [menuItem setTarget:controller];
            [menuItem setTag:flatIndex];
            [menuItem setEnabled:item.enabled ? YES : NO];
            if (item.type == TrayMenuItem::Type::Checkbox ||
                item.type == TrayMenuItem::Type::Toggle ||
                item.type == TrayMenuItem::Type::Radio) {
                [menuItem setState:item.checked ? NSControlStateValueOn : NSControlStateValueOff];
            }
            if (item.type == TrayMenuItem::Type::Submenu) {
                NSMenu* submenu = BuildMenu(item.subItems,
                                            ComputeSubmenuIndices(flatIndex,
                                                                  item.subItems.size()));
                [menuItem setSubmenu:submenu];
                // setSubmenu: 会持有子菜单，这里交出自己那一份引用。
                PPET_RELEASE(submenu);
            }
            [menu addItem:menuItem];
            // addItem: 会持有菜单项，这里交出自己那一份引用。
            PPET_RELEASE(menuItem);
        }
        return menu;
    }

    void RebuildMenu() {
        topLevelIndices = ComputeTopLevelIndices(items);
        NSMenu* menu = BuildMenu(items, topLevelIndices);
        // setMenu: 会替换并释放旧菜单，同时持有新菜单。
        [statusItem setMenu:menu];
        PPET_RELEASE(menu);
    }

    void ApplyIcon() {
        if (!statusItem) return;
        if (iconPath.empty()) {
            [statusItem.button setImage:nil];
            return;
        }
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:ToNSString(iconPath)];
        if (!image) return;
        [image setSize:NSMakeSize(18.0, 18.0)];
        [statusItem.button setImage:image];
        PPET_RELEASE(image);
    }

    void ApplyTooltip() {
        if (!statusItem) return;
        [statusItem.button setToolTip:ToNSString(tooltip)];
    }

    Tray* owner = nullptr;
};

#else

// ---------------------------------------------------------------------------
// Linux / X11 托盘实现
//
// 依赖的协议：
//   _NET_SYSTEM_TRAY_S0   —— 托盘宿主持有的选择（ayatana-indicator 协议）
//   _NET_SYSTEM_TRAY_DOCK —— 客户端向宿主发送的停靠请求
//   _NET_SYSTEM_TRAY_OPCODE —— 宿主发给客户端的操作码（显示/隐藏/点击）
//
// 所有 X11 调用都在托盘自己的线程里完成（独立的 Display 连接），
// 主线程通过命令队列与之通信，避免 Xlib 的线程安全问题。
// ---------------------------------------------------------------------------

/// 主线程投递给托盘线程的命令。
struct X11Command {
    enum class Kind { UpdateMenu, SetIcon, SetTooltip, PopupMenu, Shutdown };
    Kind kind = Kind::UpdateMenu;
    std::string text;
    std::vector<TrayMenuItem> items;
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};

// 核心 X 字体路径使用的前景色（TrueColor 视觉下的像素值）。
constexpr unsigned long kWhitePixel = 0xFFFFFFFFUL;
constexpr unsigned long kBlackPixel = 0x00000000UL;

/// X11 弹出菜单（一级菜单或子菜单）。
struct X11MenuState {
    Window window = 0;
    std::vector<int> rowIndex;    ///< 每一行对应的扁平索引（-1 表示分隔线）
    std::vector<int> rowTop;      ///< 每一行的顶部 y
    std::vector<int> rowHeight;
    int width = 0;
    int height = 0;
    int originX = 0;              ///< 窗口在虚拟桌面中的位置
    int originY = 0;
    int parentFlatIndex = -1;     ///< 若为子菜单，记录父项的扁平索引
};

struct Tray::Impl {
    // ---- X11 资源 ----
    Display* display = nullptr;
    int fd = -1;
    int screen = 0;
    Visual* visual = nullptr;
    Colormap colormap = 0;
    GC gc = nullptr;
    Window trayWindow = 0;
    Window trayOwner = 0;
    Pixmap iconPixmap = 0;
    Pixmap iconMask = 0;
    Atom traySelectionAtom = 0;
    Atom opcodeAtom = 0;
    Atom dockAtom = 0;
    bool docked = false;

#if defined(PPET_HAS_XFT)
    XftFont* xftFont = nullptr;
    XftColor xftTextColor{};
    XftColor xftHighlightColor{};
#else
    XFontStruct* coreFont = nullptr;
#endif

    // ---- 图标位图 ----
    std::vector<unsigned char> iconRgba;
    int iconWidth = 0;
    int iconHeight = 0;

    // ---- 菜单模型（仅托盘线程访问） ----
    // flatItems 是展开后的完整菜单，flatIndex 可以直接定位任意菜单项；
    // topLevelIndices 记录顶层项在 flatItems 中的位置。
    std::vector<TrayMenuItem> flatItems;
    std::vector<int> topLevelIndices;
    std::vector<int> childCounts;
    std::vector<X11MenuState> menuStack;
    int hoverRow = -1;
    int pressedRow = -1;

    // ---- 线程与同步 ----
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<X11Command> commands;
    std::deque<int> pendingSelections;  ///< 选中的扁平索引，交给主线程执行回调
    bool initDone = false;
    bool initOk = false;
    std::string initError;

    std::string iconPath;
    std::string tooltip;

    // =======================================================================
    // 初始化
    // =======================================================================

    bool OpenDisplay(std::string& error) {
        display = XOpenDisplay(nullptr);
        if (!display) {
            error = "无法打开 X display（托盘需要 X11 会话）";
            return false;
        }
        fd = ConnectionNumber(display);
        screen = DefaultScreen(display);
        visual = DefaultVisual(display, screen);
        colormap = DefaultColormap(display, screen);
        gc = XCreateGC(display, RootWindow(display, screen), 0, nullptr);
        traySelectionAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_S0", False);
        opcodeAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);
        dockAtom = XInternAtom(display, "_NET_SYSTEM_TRAY_DOCK", False);
        return true;
    }

    /// 查找系统托盘宿主窗口。
    ///
    /// 首选 XGetSelectionOwner（ayatana 协议里托盘宿主持有 _NET_SYSTEM_TRAY_S0 选择）；
    /// 退而求其次扫描根窗口子窗口，找带 MANAGER 属性的那个。
    Window FindTrayOwner() {
        Window owner = XGetSelectionOwner(display, traySelectionAtom);
        if (owner != None) return owner;

        const Atom managerAtom = XInternAtom(display, "MANAGER", False);
        const Window root = RootWindow(display, screen);
        Window rootReturn = 0;
        Window parentReturn = 0;
        Window* children = nullptr;
        unsigned int childCount = 0;
        if (!XQueryTree(display, root, &rootReturn, &parentReturn, &children, &childCount)) {
            return None;
        }
        Window found = None;
        for (unsigned int i = 0; i < childCount && found == None; ++i) {
            Atom actualType = None;
            int actualFormat = 0;
            unsigned long items = 0;
            unsigned long bytesAfter = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display, children[i], managerAtom, 0, 1, False, AnyPropertyType,
                                   &actualType, &actualFormat, &items, &bytesAfter,
                                   &data) == Success) {
                if (data) {
                    if (items > 0 && actualFormat == 32) {
                        found = reinterpret_cast<Window*>(data)[0];
                    }
                    XFree(data);
                }
            }
        }
        if (children) XFree(children);
        return found;
    }

    bool CreateTrayWindow(std::string& error) {
        const Window root = RootWindow(display, screen);
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        attributes.background_pixmap = None;
        attributes.border_pixel = 0;
        attributes.colormap = colormap;
        attributes.event_mask = StructureNotifyMask | PropertyChangeMask | ExposureMask;

        trayWindow = XCreateWindow(display, root, 0, 0, 22, 22, 0, DefaultDepth(display, screen),
                                   InputOutput, visual,
                                   CWOverrideRedirect | CWBorderPixel | CWColormap | CWEventMask,
                                   &attributes);
        if (!trayWindow) {
            error = "创建托盘图标窗口失败";
            return false;
        }

        // 尺寸提示：告诉托盘宿主我们期望的图标尺寸范围。
        XSizeHints hints{};
        hints.flags = USPosition | USSize | PMinSize | PMaxSize;
        hints.x = 0;
        hints.y = 0;
        hints.width = 22;
        hints.height = 22;
        hints.min_width = 16;
        hints.min_height = 16;
        hints.max_width = 64;
        hints.max_height = 64;
        XSetWMNormalHints(display, trayWindow, &hints);

        XClassHint classHint{};
        char className[] = "PPet";
        char classInstance[] = "PPet";
        classHint.res_name = className;
        classHint.res_class = classInstance;
        XSetClassHint(display, trayWindow, &classHint);
        return true;
    }

    /// 向托盘宿主发送停靠请求（_NET_SYSTEM_TRAY_DOCK）。
    void DockToTray() {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = trayOwner;
        event.xclient.message_type = dockAtom;
        event.xclient.format = 32;
        event.xclient.data.l[0] = CurrentTime;
        event.xclient.data.l[1] = static_cast<long>(trayWindow);
        event.xclient.data.l[2] = 0;
        XSendEvent(display, trayOwner, False, NoEventMask, &event);
        XFlush(display);
    }

    /// 等待托盘宿主确认停靠（宿主会在我们的窗口上设置 _NET_SYSTEM_TRAY_OPCODE 属性）。
    bool WaitForDock(double timeoutSeconds) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<long>(timeoutSeconds * 1000.0));
        while (std::chrono::steady_clock::now() < deadline) {
            Atom actualType = None;
            int actualFormat = 0;
            unsigned long items = 0;
            unsigned long bytesAfter = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display, trayWindow, opcodeAtom, 0, 0, False, AnyPropertyType,
                                   &actualType, &actualFormat, &items, &bytesAfter,
                                   &data) == Success) {
                if (data) {
                    if (items > 0) docked = true;
                    XFree(data);
                }
            }
            if (docked) return true;
            WaitForXEvent(0.05);
        }
        return docked;
    }

    void WaitForXEvent(double timeoutSeconds) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(timeoutSeconds);
        timeout.tv_usec =
            static_cast<suseconds_t>((timeoutSeconds - static_cast<double>(timeout.tv_sec)) *
                                     1000000.0);
        if (select(fd + 1, &fds, nullptr, nullptr, &timeout) > 0) {
            while (XPending(display) > 0) {
                XEvent event{};
                XNextEvent(display, &event);
                HandleEvent(event);
            }
        }
    }

    // =======================================================================
    // 线程主体
    // =======================================================================

    void ThreadMain() {
        std::string error;
        bool ok = OpenDisplay(error);
        if (ok) {
            trayOwner = FindTrayOwner();
            if (trayOwner == None) {
                ok = false;
                error = "未找到系统托盘宿主（_NET_SYSTEM_TRAY），已进入无托盘降级模式";
            }
        }
        if (ok) ok = CreateTrayWindow(error);
        if (ok) {
            DockToTray();
            if (!WaitForDock(1.5)) {
                ok = false;
                error = "系统托盘宿主未响应停靠请求，已进入无托盘降级模式";
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            initOk = ok;
            initError = error;
            initDone = true;
        }
        cv.notify_all();

        if (!ok) {
            CleanupX11();
            return;
        }

        LoadFont();
        RenderIcon();

        while (running.load()) {
            ProcessCommands();
            WaitForXEvent(0.05);
        }
        CleanupX11();
    }

    void CleanupX11() {
        CloseAllMenus();
        if (display) {
            if (iconPixmap) XFreePixmap(display, iconPixmap);
            if (iconMask) XFreePixmap(display, iconMask);
#if defined(PPET_HAS_XFT)
            if (xftFont) XftFontClose(display, xftFont);
            xftFont = nullptr;
#endif
            if (gc) XFreeGC(display, gc);
            if (trayWindow) XDestroyWindow(display, trayWindow);
            XCloseDisplay(display);
        }
        iconPixmap = 0;
        iconMask = 0;
        gc = nullptr;
        trayWindow = 0;
        display = nullptr;
    }

    /// 处理主线程投递的命令（只在托盘线程里执行）。
    void ProcessCommands() {
        std::deque<X11Command> local;
        {
            std::lock_guard<std::mutex> lock(mutex);
            local.swap(commands);
        }
        for (const X11Command& command : local) {
            switch (command.kind) {
                case X11Command::Kind::UpdateMenu:
                    FlattenMenuTree(command.items, &flatItems, &topLevelIndices, &childCounts);
                    CloseAllMenus();
                    break;
                case X11Command::Kind::SetIcon:
                    iconRgba = command.rgba;
                    iconWidth = command.width;
                    iconHeight = command.height;
                    RenderIcon();
                    break;
                case X11Command::Kind::SetTooltip:
                    // _NET_SYSTEM_TRAY 协议没有标准 tooltip 机制，
                    // 这里把文本作为窗口名，供部分面板显示。
                    if (trayWindow) {
                        XStoreName(display, trayWindow, command.text.c_str());
                        XFlush(display);
                    }
                    break;
                case X11Command::Kind::PopupMenu:
                    ShowRootMenu();
                    break;
                case X11Command::Kind::Shutdown:
                    running.store(false);
                    break;
            }
        }
    }

    // =======================================================================
    // 事件处理
    // =======================================================================

    void HandleEvent(XEvent& event) {
        switch (event.type) {
            case Expose:
                if (event.xexpose.window == trayWindow) RenderIcon();
                for (X11MenuState& menu : menuStack) {
                    if (event.xexpose.window == menu.window) RedrawMenu(menu);
                }
                break;
            case ConfigureNotify:
                if (event.xconfigure.window == trayWindow) RenderIcon();
                break;
            case PropertyNotify:
                if (event.xproperty.window == trayWindow &&
                    event.xproperty.atom == opcodeAtom &&
                    event.xproperty.state == PropertyNewValue) {
                    docked = true;
                }
                break;
            case DestroyNotify:
                if (event.xdestroywindow.window == trayOwner) {
                    // 托盘宿主消失：尝试重新停靠。
                    docked = false;
                    trayOwner = FindTrayOwner();
                    if (trayOwner != None) DockToTray();
                }
                break;
            case ClientMessage:
                if (event.xclient.message_type == opcodeAtom) {
                    const long opcode = event.xclient.data.l[1];
                    if (opcode == 0) {
                        // CREATE：托盘宿主让我们显示图标。
                        XMapRaised(display, trayWindow);
                        RenderIcon();
                    } else if (opcode == 1) {
                        // REMOVE：托盘宿主要求隐藏图标。
                        XUnmapWindow(display, trayWindow);
                    } else if (opcode == 2 || opcode == 3) {
                        // ACTIVATE / CONTEXT_MENU：用户点击了图标。
                        ShowRootMenu();
                    }
                }
                break;
            case ButtonPress:
                HandleButtonPress(event);
                break;
            case ButtonRelease:
                HandleButtonRelease(event);
                break;
            case MotionNotify:
                HandleMotion(event);
                break;
            case KeyPress:
                // Esc 关闭菜单。
                if (!menuStack.empty() && event.xkey.keycode == 9) CloseAllMenus();
                break;
            default:
                break;
        }
    }

    X11MenuState* FindMenuByWindow(Window window) {
        for (X11MenuState& menu : menuStack) {
            if (menu.window == window) return &menu;
        }
        return nullptr;
    }

    void HandleButtonPress(XEvent& event) {
        X11MenuState* menu = FindMenuByWindow(event.xbutton.window);
        if (menu) {
            pressedRow = RowAt(*menu, event.xbutton.y);
            return;
        }
        if (event.xbutton.window == trayWindow && event.xbutton.button == Button3) {
            ShowRootMenu();
        }
    }

    void HandleButtonRelease(XEvent& event) {
        X11MenuState* menu = FindMenuByWindow(event.xbutton.window);
        if (!menu) return;
        const int row = RowAt(*menu, event.xbutton.y);
        const int pressed = pressedRow;
        pressedRow = -1;
        const bool inside = row >= 0 && row == pressed;
        const int flatIndex = inside ? (*menu).rowIndex[static_cast<std::size_t>(row)] : -1;
        CloseAllMenus();
        if (flatIndex >= 0) {
            std::lock_guard<std::mutex> lock(mutex);
            pendingSelections.push_back(flatIndex);
        }
    }

    void HandleMotion(XEvent& event) {
        X11MenuState* menu = FindMenuByWindow(event.xmotion.window);
        if (!menu) return;
        const int row = RowAt(*menu, event.xmotion.y);
        if (row == hoverRow) return;
        hoverRow = row;
        RedrawMenu(*menu);
        // 悬停到子菜单项上时展开子菜单。
        if (row >= 0) {
            const int flatIndex = (*menu).rowIndex[static_cast<std::size_t>(row)];
            if (flatIndex >= 0 && flatIndex < static_cast<int>(flatItems.size()) &&
                flatItems[static_cast<std::size_t>(flatIndex)].type ==
                    TrayMenuItem::Type::Submenu) {
                ShowSubMenu(static_cast<int>(menuStack.size()) - 1, flatIndex,
                            (*menu).rowTop[static_cast<std::size_t>(row)]);
            }
        }
    }

    int RowAt(const X11MenuState& menu, int y) const {
        for (std::size_t i = 0; i < menu.rowTop.size(); ++i) {
            if (y >= menu.rowTop[i] && y < menu.rowTop[i] + menu.rowHeight[i]) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // =======================================================================
    // 菜单绘制
    // =======================================================================

    void LoadFont() {
#if defined(PPET_HAS_XFT)
        if (!FcInit()) return;

        // 依次尝试候选字体族，优先选择「能显示中文」的字体。
        // 注意：不能只靠字体族名列表——fontconfig 匹配到的字体可能不含 CJK 字形，
        // 那样中文会被渲染成方框或乱码，所以这里用 charset 做一次实际覆盖检查。
        static const char* const kFamilies[] = {
            "Noto Sans CJK SC", "Noto Sans SC",     "Source Han Sans SC",
            "WenQuanYi Micro Hei", "Microsoft YaHei", "PingFang SC",
            "Heiti SC",          "SimHei",          "DejaVu Sans",
            "sans-serif"};
        XftFont* fallback = nullptr;
        for (const char* family : kFamilies) {
            FcPattern* pattern = FcPatternCreate();
            if (!pattern) break;
            FcPatternAddString(pattern, FC_FAMILY,
                               reinterpret_cast<const FcChar8*>(family));
            FcPatternAddDouble(pattern, FC_SIZE, 12.0);
            FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);
            FcResult result = FcResultNoMatch;
            FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
            FcPatternDestroy(pattern);
            if (!matched) continue;

            XftFont* candidate = XftFontOpenPattern(display, matched);
            FcPatternDestroy(matched);
            if (!candidate) continue;

            // 用 U+4E00（「上」）检查是否覆盖中文字形。
            const bool hasChinese =
                candidate->charset && FcCharSetHasChar(candidate->charset, 0x4E00);
            if (hasChinese) {
                if (fallback) XftFontClose(display, fallback);
                xftFont = candidate;
                break;
            }
            if (!fallback) {
                fallback = candidate;
            } else {
                XftFontClose(display, candidate);
            }
        }
        if (!xftFont) xftFont = fallback;

        // 预先准备好前景色，避免每次绘制都去查色表。
        XftColorAllocName(display, visual, colormap, "black", &xftTextColor);
        XftColorAllocName(display, visual, colormap, "white", &xftHighlightColor);
#else
        static const char* const kFontNames[] = {"9x15", "-*-fixed-medium-r-normal--13-*",
                                                 "-*-helvetica-medium-r-normal--12-*"};
        for (const char* name : kFontNames) {
            coreFont = XLoadQueryFont(display, name);
            if (coreFont) break;
        }
#endif
    }

    /// 字体总高度（像素）。
    ///
    /// 注意：XftFont::ascent/descent 与 XFontStruct::ascent/descent 的单位都是像素，
    /// 不是 16.16 定点数——写错会导致基线被算到负数，libXft 会直接崩溃。
    int TextHeight() const {
#if defined(PPET_HAS_XFT)
        if (!xftFont) return 14;
        return xftFont->ascent + xftFont->descent;
#else
        if (!coreFont) return 14;
        return coreFont->ascent + coreFont->descent;
#endif
    }

    /// 基线相对行顶的偏移量（像素）。
    int TextAscent() const {
#if defined(PPET_HAS_XFT)
        if (!xftFont) return 11;
        return xftFont->ascent;
#else
        if (!coreFont) return 11;
        return coreFont->ascent;
#endif
    }

    int MeasureText(const std::string& text) const {
#if defined(PPET_HAS_XFT)
        if (!xftFont || text.empty()) return 0;
        // XftTextExtentsUtf8 会把整串文本的总范围写入 extents[0]，
        // 其中的 xOff 是整串的推进宽度（单位：像素）。
        // 必须用 Utf8 版本：XftTextExtents8 按 Latin-1 解释字节，中文会算错。
        XGlyphInfo extents{};
        XftTextExtentsUtf8(display, xftFont, reinterpret_cast<const FcChar8*>(text.c_str()),
                           static_cast<int>(text.size()), &extents);
        return extents.xOff;
#else
        if (!coreFont) return static_cast<int>(text.size()) * 7;
        return XTextWidth(coreFont, text.c_str(), static_cast<int>(text.size()));
#endif
    }

    void DrawText(Window target, int x, int y, const std::string& text, bool highlighted) {
#if defined(PPET_HAS_XFT)
        if (!xftFont) return;
        XftDraw* draw = XftDrawCreate(display, target, visual, colormap);
        if (!draw) return;
        const XftColor& color = highlighted ? xftHighlightColor : xftTextColor;
        // 必须用 Utf8 版本：XftDrawString8 按 Latin-1 解释每个字节，
        // UTF-8 中文会被画成乱码。
        XftDrawStringUtf8(draw, &color, xftFont, x, y,
                          reinterpret_cast<const FcChar8*>(text.c_str()),
                          static_cast<int>(text.size()));
        XftDrawDestroy(draw);
#else
        XSetForeground(display, gc, highlighted ? kWhitePixel : kBlackPixel);
        XDrawString(display, target, gc, x, y, text.c_str(), static_cast<int>(text.size()));
#endif
    }

    /// 计算菜单布局。indices 给出每一行对应的扁平索引。
    void LayoutMenu(X11MenuState& menu, const std::vector<int>& indices) {
        menu.rowIndex.clear();
        menu.rowTop.clear();
        menu.rowHeight.clear();

        const int itemHeight = TextHeight() + 8;
        int width = 0;
        int y = 4;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const int flatIndex = indices[i];
            if (flatIndex < 0 || flatIndex >= static_cast<int>(flatItems.size())) continue;
            const TrayMenuItem& item = flatItems[static_cast<std::size_t>(flatIndex)];
            if (item.type == TrayMenuItem::Type::Separator) {
                menu.rowIndex.push_back(-1);
                menu.rowTop.push_back(y);
                menu.rowHeight.push_back(6);
                y += 6;
                continue;
            }
            menu.rowIndex.push_back(flatIndex);
            menu.rowTop.push_back(y);
            menu.rowHeight.push_back(itemHeight);
            int textWidth = MeasureText(item.label);
            if (!item.accelerator.empty()) textWidth += MeasureText(item.accelerator) + 24;
            width = std::max(width, textWidth);
            y += itemHeight;
        }
        menu.height = y + 4;
        // 左侧留出勾选标记与子菜单箭头的空间。
        menu.width = std::max(width + 56, 140);
    }

    void RedrawMenu(X11MenuState& menu) {
        if (!menu.window) return;
        const unsigned long background = 0xF0F0F0;
        const unsigned long border = 0x808080;
        const unsigned long highlight = 0x316AC5;
        const unsigned long separator = 0xC0C0C0;

        XSetForeground(display, gc, background);
        XFillRectangle(display, menu.window, gc, 0, 0, menu.width, menu.height);
        XSetForeground(display, gc, border);
        XDrawRectangle(display, menu.window, gc, 0, 0, menu.width - 1, menu.height - 1);

        for (std::size_t i = 0; i < menu.rowIndex.size(); ++i) {
            const int flatIndex = menu.rowIndex[i];
            const int top = menu.rowTop[i];
            const int height = menu.rowHeight[i];
            if (flatIndex < 0) {
                // 分隔线。
                XSetForeground(display, gc, separator);
                XDrawLine(display, menu.window, gc, 4, top + height / 2, menu.width - 4,
                          top + height / 2);
                continue;
            }
            const TrayMenuItem& item = flatItems[static_cast<std::size_t>(flatIndex)];
            const bool highlighted = hoverRow == static_cast<int>(i);
            if (highlighted) {
                XSetForeground(display, gc, highlight);
                XFillRectangle(display, menu.window, gc, 2, top, menu.width - 4, height);
            }

            // 基线：让文字在行内垂直居中，且始终不低于字体的 ascent（否则会被裁掉甚至崩溃）。
            int baseline = top + TextAscent() + (height - TextHeight()) / 2;
            if (baseline < TextAscent()) baseline = TextAscent();
            // 勾选标记。
            if (item.checked) {
                XSetForeground(display, gc, highlighted ? 0xFFFFFF : 0x000000);
                const int markX = 12;
                const int markY = top + height / 2;
                XDrawLine(display, menu.window, gc, markX, markY, markX + 4, markY + 4);
                XDrawLine(display, menu.window, gc, markX + 4, markY + 4, markX + 11, markY - 5);
            }
            // 文字。
            DrawText(menu.window, 28, baseline, item.label, highlighted);
            // 快捷键提示右对齐。
            if (!item.accelerator.empty()) {
                const int accelWidth = MeasureText(item.accelerator);
                DrawText(menu.window, menu.width - accelWidth - 10, baseline, item.accelerator,
                         highlighted);
            }
            // 子菜单箭头。
            if (item.type == TrayMenuItem::Type::Submenu) {
                XSetForeground(display, gc, highlighted ? 0xFFFFFF : 0x000000);
                const int arrowX = menu.width - 14;
                const int arrowY = top + height / 2 - 3;
                XDrawLine(display, menu.window, gc, arrowX, arrowY, arrowX + 5, arrowY + 3);
                XDrawLine(display, menu.window, gc, arrowX + 5, arrowY + 3, arrowX, arrowY + 6);
            }
        }
    }

    // =======================================================================
    // 菜单弹出与关闭
    // =======================================================================

    Window CreateMenuWindow() {
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        attributes.background_pixmap = None;
        attributes.border_pixel = 0;
        attributes.colormap = colormap;
        attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                                PointerMotionMask | KeyPressMask;
        const Window window = XCreateWindow(
            display, RootWindow(display, screen), 0, 0, 200, 100, 0, DefaultDepth(display, screen),
            InputOutput, visual,
            CWOverrideRedirect | CWBorderPixel | CWColormap | CWEventMask, &attributes);
        if (window) {
            XClassHint classHint{};
            char className[] = "PPetMenu";
            char classInstance[] = "PPetMenu";
            classHint.res_name = className;
            classHint.res_class = classInstance;
            XSetClassHint(display, window, &classHint);
        }
        return window;
    }

    void ShowRootMenu() {
        if (menuStack.empty()) {
            X11MenuState menu;
            menu.window = CreateMenuWindow();
            if (!menu.window) return;
            menuStack.push_back(menu);
        }
        X11MenuState& menu = menuStack.front();
        LayoutMenu(menu, topLevelIndices);

        int cursorX = 0;
        int cursorY = 0;
        int winX = 0;
        int winY = 0;
        unsigned int mask = 0;
        Window root = 0;
        Window child = 0;
        // 根窗口坐标即虚拟桌面坐标（可能为负）。
        XQueryPointer(display, RootWindow(display, screen), &root, &child, &cursorX, &cursorY,
                      &winX, &winY, &mask);
        ShowMenuAt(menu, cursorX, cursorY);
    }

    /// 展开子菜单。parentLevel 是父菜单在 menuStack 中的层号。
    void ShowSubMenu(int parentLevel, int parentFlatIndex, int parentRowTop) {
        if (parentLevel < 0 || parentLevel >= static_cast<int>(menuStack.size())) return;
        if (parentFlatIndex < 0 || parentFlatIndex >= static_cast<int>(flatItems.size())) return;
        if (flatItems[static_cast<std::size_t>(parentFlatIndex)].type !=
            TrayMenuItem::Type::Submenu) {
            return;
        }

        // 关闭比父层更深的菜单。
        while (static_cast<int>(menuStack.size()) > parentLevel + 1) {
            DestroyMenu(menuStack.back());
            menuStack.pop_back();
        }
        // 已经展开过同一个子菜单则不重复创建。
        if (menuStack.size() > static_cast<std::size_t>(parentLevel) + 1 &&
            menuStack.back().parentFlatIndex == parentFlatIndex) {
            return;
        }

        // 子菜单项在扁平列表中紧随父项，因此索引是连续的。
        const int childCount = childCounts[static_cast<std::size_t>(parentFlatIndex)];
        std::vector<int> indices;
        indices.reserve(static_cast<std::size_t>(childCount));
        for (int i = 0; i < childCount; ++i) indices.push_back(parentFlatIndex + 1 + i);

        X11MenuState menu;
        menu.window = CreateMenuWindow();
        if (!menu.window) return;
        menu.parentFlatIndex = parentFlatIndex;
        LayoutMenu(menu, indices);
        menuStack.push_back(menu);

        X11MenuState& parentMenu = menuStack[static_cast<std::size_t>(parentLevel)];
        X11MenuState& current = menuStack.back();
        ShowMenuAt(current, parentMenu.originX + parentMenu.width - 4,
                   parentMenu.originY + parentRowTop - 4);
    }

    void ShowMenuAt(X11MenuState& menu, int x, int y) {
        const int screenWidth = DisplayWidth(display, screen);
        const int screenHeight = DisplayHeight(display, screen);
        int posX = x;
        int posY = y;
        if (posX + menu.width > screenWidth) posX = screenWidth - menu.width;
        if (posY + menu.height > screenHeight) posY = screenHeight - menu.height;
        if (posX < 0) posX = 0;
        if (posY < 0) posY = 0;

        XMoveResizeWindow(display, menu.window, posX, posY, menu.width, menu.height);
        menu.originX = posX;
        menu.originY = posY;
        XMapRaised(display, menu.window);
        RedrawMenu(menu);
        XFlush(display);

        // 抓取指针：这样在菜单外点击也能收到事件并关闭菜单。
        XGrabPointer(display, menu.window, True, ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, menu.window, None, CurrentTime);
        XGrabKeyboard(display, menu.window, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    }

    void DestroyMenu(X11MenuState& menu) {
        if (!menu.window) return;
        XUngrabPointer(display, CurrentTime);
        XUngrabKeyboard(display, CurrentTime);
        XDestroyWindow(display, menu.window);
        menu.window = 0;
    }

    void CloseAllMenus() {
        for (X11MenuState& menu : menuStack) DestroyMenu(menu);
        menuStack.clear();
        hoverRow = -1;
        pressedRow = -1;
    }

    // =======================================================================
    // 图标渲染
    // =======================================================================

    void RenderIcon() {
        if (!trayWindow || !display) return;
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, trayWindow, &attributes)) return;
        const int width = attributes.width > 0 ? attributes.width : 22;
        const int height = attributes.height > 0 ? attributes.height : 22;
        if (width <= 0 || height <= 0) return;

        if (iconPixmap) {
            XFreePixmap(display, iconPixmap);
            iconPixmap = 0;
        }
        if (iconMask) {
            XFreePixmap(display, iconMask);
            iconMask = 0;
        }

        const Window root = RootWindow(display, screen);
        iconPixmap = XCreatePixmap(display, root, width, height, DefaultDepth(display, screen));
        if (!iconPixmap) return;

        // 生成 24bpp（BGRX）像素数据。
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4, 0);
        if (!iconRgba.empty() && iconWidth > 0 && iconHeight > 0) {
            for (int y = 0; y < height; ++y) {
                // 最近邻缩放。
                const int sourceY = static_cast<int>(static_cast<long long>(y) * iconHeight /
                                                    std::max(1, height));
                for (int x = 0; x < width; ++x) {
                    const int sourceX = static_cast<int>(static_cast<long long>(x) * iconWidth /
                                                        std::max(1, width));
                    const unsigned char* src =
                        iconRgba.data() + (static_cast<std::size_t>(sourceY) * iconWidth + sourceX) * 4;
                    unsigned char* dst =
                        pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 0;
                }
            }
        } else {
            // 没有图标资源时画一个简单的圆形占位图，避免出现空白方块。
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const float dx = (x - width / 2.0f) / (width / 2.0f);
                    const float dy = (y - height / 2.0f) / (height / 2.0f);
                    const bool inside = (dx * dx + dy * dy) <= 0.8f;
                    unsigned char* dst =
                        pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                    dst[0] = inside ? 0xE0 : 0xF0;
                    dst[1] = inside ? 0x90 : 0xF0;
                    dst[2] = inside ? 0x60 : 0xF0;
                    dst[3] = 0;
                }
            }
        }

        XImage* image = XCreateImage(display, visual, DefaultDepth(display, screen), ZPixmap, 0,
                                     reinterpret_cast<char*>(pixels.data()), width, height, 32, 0);
        if (image) {
            XPutImage(display, iconPixmap, gc, image, 0, 0, 0, 0, width, height);
            image->data = nullptr;  // 避免 XDestroyImage 释放我们自己的内存
            XDestroyImage(image);
        }

        // 生成 1bpp 掩码，用 XShape 抠掉透明区域，实现带透明通道的托盘图标。
        if (!iconRgba.empty()) {
            const int stride = ((width + 31) / 32) * 4;
            std::vector<unsigned char> maskBits(static_cast<std::size_t>(stride) * height, 0);
            for (int y = 0; y < height; ++y) {
                const int sourceY = static_cast<int>(static_cast<long long>(y) * iconHeight /
                                                    std::max(1, height));
                for (int x = 0; x < width; ++x) {
                    const int sourceX = static_cast<int>(static_cast<long long>(x) * iconWidth /
                                                        std::max(1, width));
                    const unsigned char alpha =
                        iconRgba[(static_cast<std::size_t>(sourceY) * iconWidth + sourceX) * 4 + 3];
                    if (alpha <= 128) {
                        // 掩码中 1 表示透明。
                        maskBits[static_cast<std::size_t>(y) * stride + x / 8] |=
                            static_cast<unsigned char>(0x80u >> (x % 8));
                    }
                }
            }
            XImage* maskImage =
                XCreateImage(display, visual, 1, ZPixmap, 0,
                             reinterpret_cast<char*>(maskBits.data()), width, height, 32, 1);
            if (maskImage) {
                iconMask = XCreatePixmap(display, root, width, height, 1);
                if (iconMask) {
                    GC maskGc = XCreateGC(display, iconMask, 0, nullptr);
                    XPutImage(display, iconMask, maskGc, maskImage, 0, 0, 0, 0, width, height);
                    XFreeGC(display, maskGc);
                    XShapeCombineMask(display, trayWindow, ShapeBounding, 0, 0, iconMask, ShapeSet);
                }
                maskImage->data = nullptr;
                XDestroyImage(maskImage);
            }
        }

        XSetWindowBackgroundPixmap(display, trayWindow, iconPixmap);
        XClearWindow(display, trayWindow);
        XFlush(display);
    }

    Tray* owner = nullptr;
};

#endif

// ---------------------------------------------------------------------------
// Tray 公共接口
// ---------------------------------------------------------------------------

Tray::~Tray() { Destroy(); }

void Tray::RebuildFlatIndex() {
    flat_.clear();
    std::function<void(const std::vector<TrayMenuItem>&)> walk =
        [&](const std::vector<TrayMenuItem>& list) {
            for (const TrayMenuItem& item : list) {
                flat_.push_back(&item);
                if (item.type == TrayMenuItem::Type::Submenu) walk(item.subItems);
            }
        };
    walk(items_);
}

void Tray::NotifyItemSelected(int index) {
    if (index < 0 || index >= static_cast<int>(flat_.size())) return;
    const TrayMenuItem* item = flat_[static_cast<std::size_t>(index)];
    if (!item) return;
    // 分隔线不可被选中，直接忽略。
    if (item->type == TrayMenuItem::Type::Separator) return;
    // 未启用的菜单项不触发回调。
    if (!item->enabled) return;

    bool checked = item->checked;
    switch (item->type) {
        case TrayMenuItem::Type::Toggle:
        case TrayMenuItem::Type::Checkbox:
            checked = !item->checked;
            break;
        case TrayMenuItem::Type::Radio:
            checked = true;
            break;
        default:
            break;
    }
    if (callback_) callback_(item->id, checked);
}

void Tray::SetItemCallback(ItemCallback cb) { callback_ = std::move(cb); }

#if defined(_WIN32)

bool Tray::Create(std::string& error) {
    if (impl_) {
        error = "托盘已经创建";
        return false;
    }
    impl_ = new Impl();
    impl_->owner = this;

    // 注册「任务栏重建」消息（资源管理器重启后用于刷新托盘图标）。
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = Win32TrayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kTrayClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "注册托盘窗口类失败";
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    Win32TrayState& state = impl_->state;
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClassName, L"PPet", WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!state.hwnd) {
        error = "创建托盘窗口失败";
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    g_win32State = &state;

    state.nid.cbSize = sizeof(NOTIFYICONDATAW);
    state.nid.hWnd = state.hwnd;
    state.nid.uID = 1;
    state.nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    state.nid.uCallbackMessage = kTrayCallbackMessage;
    // IDI_APPLICATION 是基于窄字符（LPSTR）的资源宏，必须使用 LoadIconA；
    // 若直接用 LoadIconW 会因字符串类型不匹配而无法编译。
    state.nid.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    wcsncpy(state.nid.szTip, L"PPet", 127);
    state.nid.szTip[127] = L'\0';

    if (!Shell_NotifyIconW(NIM_ADD, &state.nid)) {
        error = "添加系统托盘图标失败";
        Destroy();
        return false;
    }
    state.iconAdded = true;

    state.onIconEvent = [this]() {
        if (impl_) impl_->ShowMenu();
    };
    state.onTaskbarCreated = [this]() {
        if (!impl_) return;
        Win32TrayState& s = impl_->state;
        if (s.iconAdded) Shell_NotifyIconW(NIM_ADD, &s.nid);
    };

    impl_->RebuildMenu();
    available_ = true;
    return true;
}

void Tray::Destroy() {
    if (!impl_) return;
    Win32TrayState& state = impl_->state;
    if (state.iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &state.nid);
        state.iconAdded = false;
    }
    if (g_win32State == &state) g_win32State = nullptr;
    if (state.hwnd) {
        DestroyWindow(state.hwnd);
        state.hwnd = nullptr;
    }
    if (impl_->menu) {
        DestroyMenu(impl_->menu);
        impl_->menu = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
    available_ = false;
}

void Tray::SetTooltip(const std::string& text) {
    if (!impl_) return;
    impl_->tooltip = text;
    impl_->ApplyTooltip();
}

void Tray::SetIcon(const std::string& path) {
    if (!impl_) return;
    impl_->iconPath = path;
    impl_->ApplyIcon();
}

void Tray::UpdateMenu(const std::vector<TrayMenuItem>& items) {
    items_ = items;
    RebuildFlatIndex();
    if (!impl_) return;
    impl_->items = items_;
    impl_->RebuildMenu();
}

void Tray::PopupMenu() {
    if (!impl_) return;
    impl_->ShowMenu();
}

void Tray::PumpEvents() {
    // Windows 的菜单事件由系统在主线程派发，无需额外处理。
}

#elif defined(__APPLE__)

bool Tray::Create(std::string& error) {
    if (impl_) {
        error = "托盘已经创建";
        return false;
    }
    impl_ = new Impl();
    impl_->owner = this;

    // 确保 NSApplication 已存在，否则状态栏项目不会显示。
    [NSApplication sharedApplication];

    impl_->controller = [[PPetTrayController alloc] init];
    impl_->controller.owner = this;
    impl_->statusItem =
        [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    if (!impl_->statusItem) {
        error = "创建状态栏项目失败";
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->RebuildMenu();
    available_ = true;
    return true;
}

void Tray::Destroy() {
    if (!impl_) return;
    if (impl_->statusItem) {
        [[NSStatusBar systemStatusBar] removeStatusItem:impl_->statusItem];
        impl_->statusItem = nil;
    }
    if (impl_->controller) {
        impl_->controller.owner = nullptr;
        PPET_RELEASE(impl_->controller);
        impl_->controller = nil;
    }
    delete impl_;
    impl_ = nullptr;
    available_ = false;
}

void Tray::SetTooltip(const std::string& text) {
    if (!impl_) return;
    impl_->tooltip = text;
    impl_->ApplyTooltip();
}

void Tray::SetIcon(const std::string& path) {
    if (!impl_) return;
    impl_->iconPath = path;
    impl_->ApplyIcon();
}

void Tray::UpdateMenu(const std::vector<TrayMenuItem>& items) {
    items_ = items;
    RebuildFlatIndex();
    if (!impl_) return;
    impl_->items = items_;
    impl_->RebuildMenu();
}

void Tray::PopupMenu() {
    // macOS 的状态栏菜单由系统弹出，此处无需处理。
}

void Tray::PumpEvents() {
    // macOS 的菜单事件在主线程派发，无需额外处理。
}

#else

bool Tray::Create(std::string& error) {
    if (impl_) {
        error = "托盘已经创建";
        return false;
    }
    impl_ = new Impl();
    impl_->owner = this;
    impl_->running.store(true);

    impl_->thread = std::thread([impl = impl_]() { impl->ThreadMain(); });

    // 等待托盘线程完成初始化（成功停靠或确认失败）。
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->cv.wait_for(lock, std::chrono::seconds(5),
                            [impl = impl_]() { return impl->initDone; });
    }

    if (!impl_->initOk) {
        const std::string reason =
            impl_->initError.empty() ? "初始化系统托盘失败" : impl_->initError;
        impl_->running.store(false);
        if (impl_->thread.joinable()) impl_->thread.join();
        delete impl_;
        impl_ = nullptr;
        error = reason;
        platformNote_ = reason;
        return false;
    }

    available_ = true;
    return true;
}

void Tray::Destroy() {
    if (!impl_) return;
    impl_->running.store(false);
    if (impl_->thread.joinable()) impl_->thread.join();
    delete impl_;
    impl_ = nullptr;
    available_ = false;
}

void Tray::SetTooltip(const std::string& text) {
    if (!impl_) return;
    impl_->tooltip = text;
    X11Command command;
    command.kind = X11Command::Kind::SetTooltip;
    command.text = text;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->commands.push_back(command);
}

void Tray::SetIcon(const std::string& path) {
    if (!impl_) return;
    impl_->iconPath = path;
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    if (!LoadPngFile(path, &rgba, &width, &height)) {
        rgba.clear();
        width = 0;
        height = 0;
    }
    X11Command command;
    command.kind = X11Command::Kind::SetIcon;
    command.rgba = std::move(rgba);
    command.width = width;
    command.height = height;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->commands.push_back(command);
}

void Tray::UpdateMenu(const std::vector<TrayMenuItem>& items) {
    items_ = items;
    RebuildFlatIndex();
    if (!impl_) return;
    X11Command command;
    command.kind = X11Command::Kind::UpdateMenu;
    command.items = items_;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->commands.push_back(command);
}

void Tray::PopupMenu() {
    if (!impl_) return;
    // Linux 上菜单由托盘线程绘制与命中测试，这里通过命令队列触发。
    X11Command command;
    command.kind = X11Command::Kind::PopupMenu;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->commands.push_back(command);
}

void Tray::PumpEvents() {
    if (!impl_) return;
    // 主线程取出托盘线程选中的菜单项并执行回调（保证回调在主线程执行）。
    std::deque<int> selections;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        selections.swap(impl_->pendingSelections);
    }
    for (int index : selections) NotifyItemSelected(index);
}

#endif

}  // namespace ppet
