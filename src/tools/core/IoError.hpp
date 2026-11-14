// PPet - C++ 移植版
// IO 错误诊断：把 std::error_code / GetLastError / errno 变成一条人类可读的、
// 能直接定位问题的中文原因（权限不足、磁盘已满、路径不存在、文件被占用……）。
//
// 【为什么需要它】
//   用户反馈「无法创建目录与文件夹，但本地工作目录 ./ppet 却可以」，过去的
//   失败文案只有「无法创建目录：X」「无法写入文件：Y」，既没有系统错误码，
//   也没有系统文本，更没说明「为什么」—— 在 %APPDATA% 指向权限受限卷、
//   可移动盘、网络驱动器时完全无法排查。
//
//   本模块把三件事统一成一行：
//     1) 系统错误码（`[5]` / `[28]`）—— 便于查文档与提 issue；
//     2) 系统原始文本（`Access is denied.` / `No space left on device`）；
//     3) 常见错误的中文原因与可操作建议。
//   所有 IO 调用点（ZIP 解压、模型缓存目录、配置落盘）复用同一份文案，
//   失败时既写进 last.log，也回填给用户可见的 error 字符串。
//
// 【设计约束】header-only 纯逻辑、零依赖（只含标准库，不 include windows.h /
//   GL / Qt），三种 CMake 配置均可编译与单测；做成 inline 是为了让直接编译
//   Config.cpp / ZipArchive.cpp 的各个测试目标无需额外链接新 .cpp。
//   错误码常量按平台条件编译，避免把 Windows 头拖进公共头。
#pragma once

#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <cerrno>
#endif

namespace ppet {
namespace io_error_detail {

#if defined(_WIN32)
// Windows GetLastError() 常见码。用字面常量而不是 include windows.h，
// 让本模块保持「零系统头依赖」。
inline constexpr int kErrFileNotFound = 2;          // ERROR_FILE_NOT_FOUND
inline constexpr int kErrPathNotFound = 3;          // ERROR_PATH_NOT_FOUND
inline constexpr int kErrAccessDenied = 5;          // ERROR_ACCESS_DENIED
inline constexpr int kErrWriteProtect = 19;         // ERROR_WRITE_PROTECT
inline constexpr int kErrNotReady = 21;             // ERROR_NOT_READY
inline constexpr int kErrSharingViolation = 32;     // ERROR_SHARING_VIOLATION
inline constexpr int kErrLockViolation = 33;        // ERROR_LOCK_VIOLATION
inline constexpr int kErrHandleDiskFull = 39;       // ERROR_HANDLE_DISK_FULL
inline constexpr int kErrBadNetpath = 53;           // ERROR_BAD_NETPATH
inline constexpr int kErrNetworkAccessDenied = 65;  // ERROR_NETWORK_ACCESS_DENIED
inline constexpr int kErrDiskFull = 112;            // ERROR_DISK_FULL
inline constexpr int kErrInvalidName = 123;         // ERROR_INVALID_NAME
inline constexpr int kErrAlreadyExists = 183;       // ERROR_ALREADY_EXISTS
inline constexpr int kErrFilenameExcedRange = 206;  // ERROR_FILENAME_EXCED_RANGE
inline constexpr int kErrDirectory = 267;           // ERROR_DIRECTORY
#endif

/// 从 FileIO 的系统错误文本里提取「错误码 N」的数字。
/// 找不到时返回 -1（老 POSIX 文本可能只含 strerror 结果）。
inline int ExtractErrorCode(std::string_view text) {
    constexpr std::string_view kMarker = "错误码";
    const std::size_t pos = text.find(kMarker);
    if (pos == std::string_view::npos) return -1;
    std::size_t i = pos + kMarker.size();
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == ':' || text[i] == '=' || text[i] == '\t')) {
        ++i;
    }
    int value = 0;
    bool any = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (text[i] - '0');
        any = true;
        ++i;
    }
    return any ? value : -1;
}

}  // namespace io_error_detail

/// 常见系统错误码对应的中文原因与建议；未知码返回空串。
/// Windows 下 code 为 GetLastError() 值，POSIX 下为 errno 值。
inline std::string IoErrorHint(int code) {
#if defined(_WIN32)
    using namespace io_error_detail;
    switch (code) {
        case kErrAccessDenied:
            return "权限不足：当前账户无权写入该位置，或目录被只读属性 / 安全软件拦截；"
                   "可尝试以管理员身份运行，或把目录换到用户可写位置";
        case kErrWriteProtect:
            return "介质被写保护（只读磁盘 / U 盘写保护开关）";
        case kErrNotReady:
            return "设备未就绪：软驱、可移动盘或网络驱动器尚未挂载完成";
        case kErrSharingViolation:
            return "文件被其它进程占用（共享冲突），如杀毒扫描或尚未释放的句柄";
        case kErrLockViolation:
            return "文件被其它进程锁定，请关闭占用程序后重试";
        case kErrDiskFull:
        case kErrHandleDiskFull:
            return "磁盘空间已满，请清理目标磁盘后重试";
        case kErrPathNotFound:
            return "路径不存在：上级目录缺失或盘符无效，程序会先尝试逐级创建目录";
        case kErrFileNotFound:
            return "系统找不到指定的文件";
        case kErrBadNetpath:
            return "网络路径不可达，请检查网络驱动器映射";
        case kErrNetworkAccessDenied:
            return "网络访问权限被拒绝";
        case kErrInvalidName:
            return "文件名 / 目录名语法非法（含非法字符或保留字）";
        case kErrDirectory:
            return "目录名无效";
        case kErrFilenameExcedRange:
            return "路径过长（Windows 默认上限约 260 字符），请缩短模型包路径";
        case kErrAlreadyExists:
            return "同名文件或目录已存在";
        default:
            return std::string();
    }
#else
    switch (code) {
        case EACCES:
            return "权限不足：当前账户无权写入该位置；检查目录权限与只读属性";
        case EPERM:
            return "操作不被允许（权限或文件系统策略限制）";
        case ENOENT:
            return "路径不存在：上级目录缺失，程序会先尝试逐级创建目录";
        case ENOTDIR:
            return "路径中某一级不是目录（被同名文件占用）";
        case ENOSPC:
            return "磁盘空间已满，请清理目标磁盘后重试";
#ifdef EDQUOT
        case EDQUOT:
            return "磁盘配额已满";
#endif
        case EROFS:
            return "文件系统为只读（介质写保护或只读挂载）";
        case EISDIR:
            return "目标是目录而不是文件";
        case ENAMETOOLONG:
            return "文件名或路径过长";
        case EBUSY:
            return "设备或资源正忙（被其它进程占用 / 仍被挂载）";
        case EIO:
            return "底层 IO 错误（磁盘或介质异常）";
        case EMFILE:
        case ENFILE:
            return "打开的文件句柄过多";
        default:
            return std::string();
    }
#endif
}

/// 格式化 std::error_code：`[5] Access is denied.（权限不足：…）`
/// ec 为 0（没有错误）时返回明确占位，绝不返回空串造成「静默失败」。
inline std::string IoErrorText(const std::error_code& ec) {
    if (!ec) return "未知原因（系统未返回错误码；请检查目标磁盘、权限与安全软件拦截）";
    std::string out = "[" + std::to_string(ec.value()) + "] " + ec.message();
    const std::string hint = IoErrorHint(ec.value());
    if (!hint.empty()) out += "（" + hint + "）";
    return out;
}

/// 规范化 IO 层（FileIO 的 outError）返回的系统错误文本。
/// FileIO 的文本已含系统原文与「错误码 N」；这里补齐中文提示。
/// 空文本时给出可操作的占位说明，避免只剩一句「失败」。
inline std::string IoErrorText(std::string_view sysError) {
    if (sysError.empty()) {
        return "未知原因（IO 层未返回系统错误文本；请检查目标磁盘、权限与安全软件拦截）";
    }
    std::string out(sysError);
    const int code = io_error_detail::ExtractErrorCode(sysError);
    if (code >= 0) {
        const std::string hint = IoErrorHint(code);
        if (!hint.empty()) out += "（" + hint + "）";
    }
    return out;
}

/// 拼一条完整的 IO 失败描述：`操作：路径（原因）`。
/// 原因为空时用占位文本，保证任何失败都有可诊断信息。
inline std::string DescribeIoFailure(std::string_view operation, std::string_view path,
                                     std::string_view reason) {
    std::string out(operation);
    if (!path.empty()) {
        out += "：";
        out.append(path);
    }
    out += "（";
    out.append(reason.empty() ? std::string_view("未提供系统原因") : reason);
    out += "）";
    return out;
}

}  // namespace ppet
