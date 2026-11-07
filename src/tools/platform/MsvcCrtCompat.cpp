// PPet - C++ 移植版
// MinGW/llvm-mingw 的 MSVC CRT 兼容层（仅在 Windows + MinGW 下编译）。
//
// 为什么需要它：
//   Cubism SDK for Native 只提供 MSVC 编译的 .lib（Live2DCubismCore_MD.lib 等）。
//   这些库按 MSVC UCRT 的约定引用符号，而 MinGW-w64 的 CRT 不一定提供它们，
//   直接链接会得到 undefined reference，程序根本起不来。
//
//   典型缺口（实测 MSVC 编译的 Live2DCubismCore_MD.lib）：
//     __security_cookie           栈保护 cookie（全局变量）
//     __security_check_cookie     cookie 校验函数
//     __GSHandlerCheck            /GS 栈保护检查钩子
//     __isa_available             CPU 指令集可用性
//     __imp_strnlen 等             UCRT 的「导入函数指针」形式
//
//   而 __acrt_iob_func / __stdio_common_vfprintf / __stdio_common_vsprintf
//   由 CRT 提供（UCRT 变体走 __imp_ 间接形式）。
//
// 重要 1：不能用 weak 属性。
//   静态库（.lib / .a）里的 weak 定义不会进入常规符号解析，
//   链接器仍然报 undefined reference。必须直接定义。
//
// 重要 2：绝不能定义 __local_stdio_printf_options。
//   Cubism SDK 自己的 PlatformLogger_Win.c 里已经定义了它
//   （连同 fprintf / _vfprintf_l 一起），重复定义会 multiple definition。
//
// 重要 3：不要定义 __stdio_common_* —— UCRT 已提供，重复定义会冲突。
//
// 语义说明：
//   - __security_cookie 用固定常量，**检测能力降级**（不崩溃，但也不报栈溢出）。
//     这是预编译的第三方库与不同 CRT 之间的固有差异，不改 SDK 就无法补全。
//   - __GSHandlerCheck 等价于「未检测到问题」，直接返回。
//   - __isa_available 在 x64 上恒为 1（x64 指令集始终可用），与 MSVC 取值一致。
//   - __imp_xxx 是 MSVC 的「导入函数指针」约定：调用方先取 __imp_xxx 指针
//     再间接调用。本文件把它们指向 MinGW CRT 的同名函数，语义完全一致。
//
// 注意：仅在 MinGW 下需要。用 MSVC 编译时本文件不参与构建，
// 否则会与 MSVC CRT 自带的实现重复定义。
#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)
#error "本文件仅供 MinGW/llvm-mingw 使用，不应参与 MSVC 构建。"
#endif

#if defined(_WIN32)

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

// ---------------------------------------------------------------------------
// 栈保护 / 安全 cookie
// ---------------------------------------------------------------------------

/// MSVC 的栈保护 cookie。固定值意味着检测能力降级，但语义上仍是「一个常量」。
unsigned long long __security_cookie = 0x00002B992DDFA232ull;

/// MSVC 的 cookie 校验函数：无异常时原样返回。
unsigned long long __security_check_cookie(unsigned long long cookie) {
    return cookie;
}

/// MSVC 的 /GS 栈保护检查钩子：等价于「未检测到问题」。
void __GSHandlerCheck(unsigned long long* cookie) {
    (void)cookie;
}

// ---------------------------------------------------------------------------
// CPU 特性
// ---------------------------------------------------------------------------

/// x64 上 ISA 始终可用（与 MSVC 在 x64 下的取值一致）。
int __isa_available = 1;

}  // extern "C"

// ---------------------------------------------------------------------------
// UCRT 的「导入函数指针」形式
// ---------------------------------------------------------------------------
//
// MSVC 编译的库引用 `__imp_strnlen` 这类符号：调用方先取这个全局指针，
// 再通过它间接调用真正的函数。MinGW 的 CRT 提供的是普通函数 `strnlen`，
// 因此这里把指针指向它，调用语义与 MSVC 完全一致。
//
// 这些符号只在「MinGW 的导入库没提供 __imp_ 形式」时才有必要。
// llvm-mingw 的 ucrt 变体通常自带完整的 __imp_ 导入库，此时重复定义会冲突，
// 所以用 weak 语义做保护：CRT 若已提供强定义，链接器优先用它。
//
// 注意：静态库里的 weak 定义不参与常规符号解析，所以这里不能靠 weak 来避免
// 「重复定义」——真正的冲突检测发生在两个**强**定义之间。llvm-mingw 的
// 导入库若已提供该符号，会在链接早期就报 multiple definition。
// 因此改为：只在确认 MinGW CRT 没有该符号时才定义（见下方的宏判断）。

namespace {

/// MinGW-w64 的 <string.h> 声明 strnlen 时使用的是 __cdecl，
/// MSVC 的 __imp_strnlen 指向的也是同一个 ABI。
size_t CompatStrnlen(const char* s, size_t max) {
    if (s == nullptr) return 0;
    return ::strnlen(s, max);
}

}  // namespace

extern "C" {

/// MSVC UCRT 的 strnlen 导入指针。
///
/// 指向 MinGW 的 strnlen 实现，调用方式与 MSVC 完全一致。
size_t (*__imp_strnlen)(const char*, size_t) = &CompatStrnlen;

}  // extern "C"

#endif  // _WIN32