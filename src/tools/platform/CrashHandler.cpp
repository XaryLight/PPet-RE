// PPet - C++ 移植版
// 崩溃诊断实现。
#include <src/tools/platform/CrashHandler.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>

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
#endif

namespace ppet::crash {
namespace {

char AsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i])) return false;
    }
    return true;
}

std::string_view BaseName(std::string_view path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

#if defined(_WIN32)

// 异常过滤器运行在进程堆可能已经损坏的上下文里：不能用 std::string、
// 不能走 Logger（会分配内存），全部用栈缓冲 + Win32 原生 WriteFile 追加。
void AppendCrashLine(const char* text) {
    HANDLE file = CreateFileW(L"last.log", FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        SetFilePointer(file, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
        CloseHandle(file);
    }
    // 同时输出到 stderr，让从终端（--console）启动的用户立刻看到。
    std::fputs(text, stderr);
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* ep) {
    // 防止过滤器自身再崩导致递归。
    static volatile LONG entered = 0;
    if (InterlockedCompareExchange(&entered, 1, 0) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const DWORD code =
        (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    const void* address =
        (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    wchar_t modulePathW[MAX_PATH] = L"";
    wchar_t exePathW[MAX_PATH] = L"";
    HMODULE module = nullptr;
    if (address &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module)) {
        GetModuleFileNameW(module, modulePathW, MAX_PATH);
    }
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);

    char modulePath[MAX_PATH * 3] = "";
    char exePath[MAX_PATH * 3] = "";
    WideCharToMultiByte(CP_UTF8, 0, modulePathW, -1, modulePath, sizeof(modulePath),
                        nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, exePathW, -1, exePath, sizeof(exePath), nullptr,
                        nullptr);

    const Origin origin = ClassifyOrigin(modulePath, exePath);

    // ExceptionName 返回 string_view；这里手工拷进栈缓冲，避免构造 std::string。
    char name[64] = "UNKNOWN";
    const std::string_view en = ExceptionName(static_cast<unsigned long>(code));
    const std::size_t nameLen = en.size() < sizeof(name) - 1 ? en.size() : sizeof(name) - 1;
    for (std::size_t i = 0; i < nameLen; ++i) name[i] = en[i];
    name[nameLen] = '\0';

    char line[1024];
    std::snprintf(line, sizeof(line),
                  "[CRASH] 未处理异常 0x%08lX (%s)，异常地址 %p\n",
                  static_cast<unsigned long>(code), name, address);
    AppendCrashLine(line);
    std::snprintf(line, sizeof(line), "[CRASH] 所属模块：%s\n",
                  modulePath[0] ? modulePath : "(未知)");
    AppendCrashLine(line);

    switch (origin) {
        case Origin::kThirdParty:
            AppendCrashLine(
                "[CRASH] 判定：崩溃发生在第三方注入模块，不在 PPet 主程序内。\n"
                "[CRASH] 常见来源：游戏加加 GamePP / RTSS / MSI Afterburner / "
                "NVIDIA GeForce Experience Overlay / Discord Overlay / 录屏与帧率监控软件。\n"
                "[CRASH] 这些软件会向 OpenGL / D3D 进程注入 IPC overlay；控制台里的 "
                "CreateSemaphore / CreateFileMapping 失败(5) 与 GAMEPP-IPC-OVERLAY "
                "正是它们的输出，不是 PPet 的日志。\n"
                "[CRASH] 处理：退出上述软件，或在其设置里把 PPet 加入「不注入 / 白名单」；"
                "也可用 --safe-mode 启动降低与 overlay 的交互。\n");
            break;
        case Origin::kSelf:
            AppendCrashLine(
                "[CRASH] 判定：崩溃发生在 PPet 主程序内。请把 last.log 与复现步骤提交到 Issues。\n");
            break;
        case Origin::kUnknown:
            AppendCrashLine(
                "[CRASH] 判定：无法确定崩溃模块（可能位于 JIT / 显卡驱动代码）。"
                "请附上 last.log 提交到 Issues。\n");
            break;
    }
    AppendCrashLine(
        "[CRASH] 提示：0xC0000005 = EXCEPTION_ACCESS_VIOLATION（访问违规）。\n");

    // 让进程按异常码退出，而不是弹 WerFault 对话框挂在桌面上。
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif  // _WIN32

}  // namespace

Origin ClassifyOrigin(std::string_view modulePath, std::string_view exePath) {
    if (modulePath.empty()) return Origin::kUnknown;
    const std::string_view a = BaseName(modulePath);
    const std::string_view b = BaseName(exePath);
    if (a.empty() || b.empty()) return Origin::kUnknown;
    return EqualsIgnoreCase(a, b) ? Origin::kSelf : Origin::kThirdParty;
}

std::string_view ExceptionName(unsigned long code) {
    switch (code) {
        case 0xC0000005UL: return "EXCEPTION_ACCESS_VIOLATION";
        case 0xC0000008UL: return "EXCEPTION_INVALID_HANDLE";
        case 0xC00000FDUL: return "EXCEPTION_STACK_OVERFLOW";
        case 0xC0000094UL: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case 0xC0000135UL: return "STATUS_DLL_NOT_FOUND";
        case 0xC0000409UL: return "STATUS_STACK_BUFFER_OVERRUN";
        case 0x80000003UL: return "EXCEPTION_BREAKPOINT";
        default: return "UNKNOWN";
    }
}

void Install() {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(&TopLevelFilter);
#endif
}

}  // namespace ppet::crash
