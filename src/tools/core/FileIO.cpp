// PPet - C++ 移植版
// 跨平台文件读写辅助实现。详见 FileIO.hpp 的说明。
#include <src/tools/core/FileIO.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ppet {
namespace {

#if defined(_WIN32)

/// UTF-8 → UTF-16（Windows 宽字符 API 需要）。
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                            static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        wide.data(), needed);
    return wide;
}

/// UTF-16 → UTF-8。
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                            static_cast<int>(wide.size()), nullptr, 0,
                                            nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

#endif  // _WIN32

/// 取最近一次系统调用的错误文本（必须紧接在失败的 API 之后调用）。
///
/// Windows 读 GetLastError()、POSIX 读 errno，都转成 UTF-8 串。
/// 之所以单独成函数：格式化错误文本本身会触发其它系统调用、可能清掉
/// LastError/errno，所以调用方务必在失败点「立刻」取一次，不要隔行再读。
std::string OsErrorText() {
#if defined(_WIN32)
    const DWORD code = GetLastError();
    if (code == 0) return std::string();
    LPWSTR buf = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (len == 0 || buf == nullptr) return "系统错误码 " + std::to_string(code);
    std::wstring msg(buf, len);
    LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) {
        msg.pop_back();
    }
    return WideToUtf8(msg) + "（错误码 " + std::to_string(code) + "）";
#else
    return std::strerror(errno);
#endif
}

}  // namespace

std::string ExecutableDir() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                 static_cast<DWORD>(buf.size()));
    while (n == buf.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buf.resize(buf.size() * 2);
        n = GetModuleFileNameW(nullptr, buf.data(),
                               static_cast<DWORD>(buf.size()));
    }
    if (n == 0) return std::string();
    std::wstring path(buf, n);
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return WideToUtf8(slash == std::wstring::npos ? path : path.substr(0, slash));
#elif defined(__linux__)
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    const std::string path(buf);
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(0, slash);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
    char resolved[4096];
    if (realpath(buf, resolved) == nullptr) return std::string(buf);
    const std::string path(resolved);
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(0, slash);
#else
    return std::string();
#endif
}

std::string DecodeAnsiToUtf8(const std::string& ansi) {
    if (ansi.empty()) return std::string();
#if defined(_WIN32)
    // CP_ACP = 系统当前 ANSI 代码页（中文系统通常是 GBK/936）。
    const int needed = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(),
                                            static_cast<int>(ansi.size()), nullptr, 0);
    if (needed <= 0) return std::string();
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), static_cast<int>(ansi.size()),
                        wide.data(), needed);
    return WideToUtf8(wide);
#else
    // 其它平台的字节串本身即 UTF-8（POSIX 文件名不做转码）。
    return ansi;
#endif
}

std::string GetEnvVar(const char* name) {
    if (name == nullptr || *name == '\0') return std::string();
#if defined(_WIN32)
    // 宽字符 API：拿到的是 UTF-16，转成 UTF-8 后与项目内部表示一致。
    const std::wstring wideName = Utf8ToWide(name);
    if (wideName.empty()) return std::string();

    // 首次调用只查询所需长度。
    SetLastError(ERROR_SUCCESS);
    const DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed == 0) return std::string();

    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), needed);
    if (written == 0 || written >= needed) {
        // 环境变量在两次调用之间发生了变化，重试一次。
        SetLastError(ERROR_SUCCESS);
        const DWORD again = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
        if (again == 0) return std::string();
        value.assign(again, L'\0');
        if (GetEnvironmentVariableW(wideName.c_str(), value.data(), again) == 0) {
            return std::string();
        }
    }
    // 结尾的 NUL 不算内容。
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return WideToUtf8(value);
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::filesystem::path ToFsPath(const std::string& utf8Path) {
#if defined(_WIN32)
    // 显式走 UTF-8 -> UTF-16，避免 MSVC 按 ANSI 代码页解释窄字符串。
    const std::wstring widePath = Utf8ToWide(utf8Path);
    if (widePath.empty()) return std::filesystem::path();
    return std::filesystem::path(widePath);
#else
    return std::filesystem::path(utf8Path);
#endif
}

std::string FromFsPath(const std::filesystem::path& path) {
#if defined(_WIN32)
    // 显式走 UTF-16 -> UTF-8，避免 path::string() 按 ANSI 代码页转换。
    return WideToUtf8(path.wstring());
#else
    return path.string();
#endif
}

std::string FromFsPathGeneric(const std::filesystem::path& path) {
    std::string s = FromFsPath(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool ReadFileBytes(const std::string& path, std::vector<std::uint8_t>& out,
                   std::string* outError) {
    if (path.empty()) {
        if (outError) *outError = "路径为空";
        return false;
    }

#if defined(_WIN32)
    // Windows：必须走宽字符 API，否则含中文的路径会因 ANSI 代码页不匹配而失败。
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        if (outError) *outError = "路径转 UTF-16 失败";
        return false;
    }

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (outError) *outError = OsErrorText();
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        const std::string err = OsErrorText();
        CloseHandle(handle);
        if (outError) *outError = err;
        return false;
    }
    if (size.QuadPart < 0) {
        CloseHandle(handle);
        if (outError) *outError = "文件大小非法";
        return false;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < buffer.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size() - total, 64u * 1024u));
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data() + total, chunk, &read, nullptr) ||
            read == 0) {
            const std::string err = OsErrorText();
            CloseHandle(handle);
            if (outError) *outError = err;
            return false;
        }
        total += read;
    }
    CloseHandle(handle);
    out.swap(buffer);
    return true;
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (outError) *outError = OsErrorText();
        return false;
    }

    std::vector<std::uint8_t> buffer;
    std::uint8_t chunk[64 * 1024];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        buffer.insert(buffer.end(), chunk, chunk + read);
    }
    const bool ok = std::ferror(file) == 0;
    const int saveErrno = errno;
    std::fclose(file);
    if (!ok) {
        if (outError) {
            *outError = saveErrno != 0 ? std::strerror(saveErrno)
                                       : "读取不完整（文件被截断或被占用）";
        }
        return false;
    }
    out.swap(buffer);
    return true;
#endif
}

bool ReadFileText(const std::string& path, std::string& out,
                  std::string* outError) {
    std::vector<std::uint8_t> bytes;
    if (!ReadFileBytes(path, bytes, outError)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool WriteFileText(const std::string& path, const std::string& text,
                   std::string* outError) {
    if (path.empty()) {
        if (outError) *outError = "路径为空";
        return false;
    }

#if defined(_WIN32)
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        if (outError) *outError = "路径转 UTF-16 失败";
        return false;
    }

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (outError) *outError = OsErrorText();
        return false;
    }

    bool ok = true;
    std::size_t total = 0;
    while (total < text.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(text.size() - total, 64u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, text.data() + total, chunk, &written, nullptr) ||
            written == 0) {
            if (outError) *outError = OsErrorText();
            ok = false;
            break;
        }
        total += written;
    }
    CloseHandle(handle);
    return ok;
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        if (outError) *outError = OsErrorText();
        return false;
    }
    const std::size_t written =
        text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
    const bool ok = written == text.size();
    const int saveErrno = errno;
    std::fclose(file);
    if (!ok) {
        if (outError) {
            *outError = saveErrno != 0 ? std::strerror(saveErrno)
                                       : "写入字节数不足（磁盘已满或被截断）";
        }
        return false;
    }
    return ok;
#endif
}

bool WriteFileBytes(const std::string& path, const std::vector<std::uint8_t>& data,
                   std::string* outError) {
    if (path.empty()) {
        if (outError) *outError = "路径为空";
        return false;
    }

#if defined(_WIN32)
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        if (outError) *outError = "路径转 UTF-16 失败";
        return false;
    }

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (outError) *outError = OsErrorText();
        return false;
    }

    bool ok = true;
    std::size_t total = 0;
    while (total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - total, 64u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + total, chunk, &written, nullptr) ||
            written == 0) {
            if (outError) *outError = OsErrorText();
            ok = false;
            break;
        }
        total += written;
    }
    CloseHandle(handle);
    return ok;
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        if (outError) *outError = OsErrorText();
        return false;
    }
    const std::size_t written =
        data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), file);
    const bool ok = written == data.size();
    const int saveErrno = errno;
    std::fclose(file);
    if (!ok) {
        if (outError) {
            *outError = saveErrno != 0 ? std::strerror(saveErrno)
                                       : "写入字节数不足（磁盘已满或被截断）";
        }
        return false;
    }
    return ok;
#endif
}

bool FileExists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;
    const DWORD attrs = GetFileAttributesW(widePath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
#endif
}

std::string ParentDirectory(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return std::string(1, path[0]);
    return path.substr(0, slash);
}

std::string FileExtension(const std::string& path) {
    const std::string name = FileName(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string FileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

}  // namespace ppet
