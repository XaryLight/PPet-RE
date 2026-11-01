// PPet - C++ 移植版
// 用系统默认程序打开外部链接：平台实现。
#include "platform/OpenExternal.hpp"

#include <cctype>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#elif defined(__APPLE__)
#import <AppKit/AppKit.h>

#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ppet {
namespace {

/// 判断是否以指定前缀开头（忽略大小写）。
bool StartsWithIgnoreCase(const std::string& text, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    if (text.size() < length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

#if defined(_WIN32)

/// UTF-8 → UTF-16。
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

#elif !defined(__APPLE__)

/// 在 PATH 中查找可执行文件。
bool FindInPath(const std::string& program, std::string& fullPath) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;
    const std::string path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find(':', start);
        const std::string directory = path.substr(start, end == std::string::npos
                                                            ? std::string::npos
                                                            : end - start);
        if (!directory.empty()) {
            const std::string candidate = directory + "/" + program;
            if (access(candidate.c_str(), X_OK) == 0) {
                fullPath = candidate;
                return true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

/// 不经过 shell 直接启动程序（避免 URL 中的特殊字符被 shell 解释）。
/// @return 进程是否成功启动
bool LaunchProgram(const std::string& program, const std::string& argument) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // 子进程：脱离终端，避免阻塞父进程。
        setsid();
        const char* argv[] = {program.c_str(), argument.c_str(), nullptr};
        execv(program.c_str(), const_cast<char* const*>(argv));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

}  // namespace

bool OpenExternalUrl(const std::string& url, std::string& error) {
    if (url.empty()) {
        error = "链接为空";
        return false;
    }
    // 只放行 http/https，其它协议（file://、javascript: 等）一律拒绝。
    if (!StartsWithIgnoreCase(url, "http://") && !StartsWithIgnoreCase(url, "https://")) {
        error = "仅支持 http/https 链接";
        return false;
    }

#if defined(_WIN32)
    const std::wstring wide = ToWide(url);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    // ShellExecute 成功时返回值 > 32。
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        error = "无法打开系统浏览器";
        return false;
    }
    return true;
#elif defined(__APPLE__)
    NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!target) {
        error = "链接格式无效";
        return false;
    }
    if (![[NSWorkspace sharedWorkspace] openURL:target]) {
        error = "无法打开系统浏览器";
        return false;
    }
    return true;
#else
    // Linux：按可用性依次尝试常见的打开器。
    static const char* const kOpeners[] = {"xdg-open", "gio", "gnome-open", "kde-open",
                                           "x-www-browser", "www-browser", "sensible-browser"};
    for (const char* opener : kOpeners) {
        std::string fullPath;
        if (!FindInPath(opener, fullPath)) continue;
        if (LaunchProgram(fullPath, url)) return true;
    }
    error = "未找到可用的 URL 打开程序（xdg-open / gio / x-www-browser）";
    return false;
#endif
}

}  // namespace ppet
