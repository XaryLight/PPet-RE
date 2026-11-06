// PPet - C++ 移植版
// 跨平台文件读写辅助实现。详见 FileIO.hpp 的说明。
#include "core/FileIO.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

#endif  // _WIN32

}  // namespace

bool ReadFileBytes(const std::string& path, std::vector<std::uint8_t>& out) {
    if (path.empty()) return false;

#if defined(_WIN32)
    // Windows：必须走宽字符 API，否则含中文的路径会因 ANSI 代码页不匹配而失败。
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        CloseHandle(handle);
        return false;
    }
    if (size.QuadPart < 0) {
        CloseHandle(handle);
        return false;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < buffer.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size() - total, 64u * 1024u));
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data() + total, chunk, &read, nullptr) || read == 0) {
            CloseHandle(handle);
            return false;
        }
        total += read;
    }
    CloseHandle(handle);
    out.swap(buffer);
    return true;
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;

    std::vector<std::uint8_t> buffer;
    std::uint8_t chunk[64 * 1024];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        buffer.insert(buffer.end(), chunk, chunk + read);
    }
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) return false;
    out.swap(buffer);
    return true;
#endif
}

bool ReadFileText(const std::string& path, std::string& out) {
    std::vector<std::uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool WriteFileText(const std::string& path, const std::string& text) {
    if (path.empty()) return false;

#if defined(_WIN32)
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    std::size_t total = 0;
    while (total < text.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(text.size() - total, 64u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, text.data() + total, chunk, &written, nullptr) ||
            written == 0) {
            ok = false;
            break;
        }
        total += written;
    }
    CloseHandle(handle);
    return ok;
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const std::size_t written =
        text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), file);
    const bool ok = written == text.size();
    std::fclose(file);
    return ok;
#endif
}

bool WriteFileBytes(const std::string& path, const std::vector<std::uint8_t>& data) {
    if (path.empty()) return false;

#if defined(_WIN32)
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) return false;

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    std::size_t total = 0;
    while (total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - total, 64u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + total, chunk, &written, nullptr) ||
            written == 0) {
            ok = false;
            break;
        }
        total += written;
    }
    CloseHandle(handle);
    return ok;
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const std::size_t written =
        data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), file);
    const bool ok = written == data.size();
    std::fclose(file);
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
