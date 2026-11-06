// PPet - C++ 移植版
// 开机启动设置：平台实现。
#include "platform/AutoStart.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>

#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ppet {
namespace {

/// 开机启动项的标识（注册表值名 / plist 文件名 / desktop 文件名）。
constexpr const char* kAutoStartName = "PPet";

#if !defined(_WIN32)
/// plist / desktop 文件用的标识（Windows 走注册表，不需要）。
constexpr const char* kAutoStartLabel = "com.ppet.ppet";
#endif

#if defined(_WIN32)

constexpr const wchar_t* kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

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

/// UTF-16 → UTF-8。
std::string FromWide(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return std::string();
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0],
                        size, nullptr, nullptr);
    return result;
}

/// 读取注册表里的开机启动命令。
bool ReadRegistryValue(std::wstring& value) {
    value.clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[2048] = {0};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, ToWide(kAutoStartName).c_str(), nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return false;
    value = buffer;
    return true;
}

#elif defined(__APPLE__)

/// 用户主目录。
std::string HomeDirectory() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : std::string();
}

std::string LaunchAgentPath() {
    return HomeDirectory() + "/Library/LaunchAgents/" + kAutoStartLabel + ".plist";
}

bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

/// 从 plist 中提取 ProgramArguments 数组里的第一个字符串。
std::string ParseLaunchAgentProgram(const std::string& path) {
    std::ifstream file(path);
    if (!file) return std::string();
    std::string line;
    while (std::getline(file, line)) {
        // 形如：  <string>/path/to/PPet</string>
        const std::size_t open = line.find("<string>");
        if (open == std::string::npos) continue;
        const std::size_t valueStart = open + 8;
        const std::size_t close = line.find("</string>", valueStart);
        if (close == std::string::npos) continue;
        return line.substr(valueStart, close - valueStart);
    }
    return std::string();
}

/// 逐级创建目录（等价于 mkdir -p）。
bool EnsureDirectory(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (current == "/" || current.empty()) continue;
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

#else

/// 用户主目录。
std::string HomeDirectory() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : std::string();
}

std::string AutostartDirectory() { return HomeDirectory() + "/.config/autostart"; }

std::string AutostartFilePath() { return AutostartDirectory() + "/" + kAutoStartName + ".desktop"; }

bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

/// 逐级创建目录（等价于 mkdir -p）。
bool EnsureDirectory(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (current == "/" || current.empty()) continue;
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

/// 从 .desktop 文件的 Exec= 行中取出程序路径（去掉可能的参数与引号）。
std::string ParseDesktopExec(const std::string& path) {
    std::ifstream file(path);
    if (!file) return std::string();
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("Exec=", 0) != 0) continue;
        std::string value = line.substr(5);
        // 去掉引号
        if (!value.empty() && value.front() == '"') {
            const std::size_t end = value.find('"', 1);
            if (end != std::string::npos) return value.substr(1, end - 1);
        }
        // 去掉参数
        const std::size_t space = value.find(' ');
        if (space != std::string::npos) value = value.substr(0, space);
        return value;
    }
    return std::string();
}

#endif

}  // namespace

bool GetExecutablePath(std::string& path) {
    path.clear();
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    path = FromWide(std::wstring(buffer, length));
    return !path.empty();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return false;
    path = buffer.data();
    return !path.empty();
#else
    std::vector<char> buffer(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return false;
    path.assign(buffer.data(), static_cast<std::size_t>(length));
    return !path.empty();
#endif
}

bool IsAutoStartEnabled(std::string& exePath) {
    exePath.clear();

#if defined(_WIN32)
    std::wstring registered;
    if (!ReadRegistryValue(registered)) return false;
    exePath = FromWide(registered);

    // 注册表里可能有引号，比较前先去掉。
    std::string normalized = exePath;
    if (!normalized.empty() && normalized.front() == '"' && normalized.back() == '"') {
        normalized = normalized.substr(1, normalized.size() - 2);
    }
    std::string current;
    if (!GetExecutablePath(current)) return false;
    // Windows 路径不区分大小写。
    if (normalized.size() != current.size()) return false;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(normalized[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(current[i])));
        if (a != b) return false;
    }
    return true;
#elif defined(__APPLE__)
    const std::string path = LaunchAgentPath();
    if (!FileExists(path)) return false;
    exePath = ParseLaunchAgentProgram(path);
    std::string current;
    if (!GetExecutablePath(current)) return false;
    return !exePath.empty() && exePath == current;
#else
    const std::string path = AutostartFilePath();
    if (!FileExists(path)) return false;
    exePath = ParseDesktopExec(path);
    std::string current;
    if (!GetExecutablePath(current)) return false;
    return !exePath.empty() && exePath == current;
#endif
}

bool SetAutoStart(bool enabled, std::string& error) {
    std::string exePath;
    if (!GetExecutablePath(exePath)) {
        error = "无法获取当前程序路径";
        return false;
    }

#if defined(_WIN32)
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        error = "无法打开注册表启动项";
        return false;
    }
    if (enabled) {
        // 路径含空格时用引号包起来，避免命令行解析出错。
        const std::wstring value = L"\"" + ToWide(exePath) + L"\"";
        const LSTATUS status = RegSetValueExW(key, ToWide(kAutoStartName).c_str(), 0,
                                               REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                               static_cast<DWORD>((value.size() + 1) *
                                                                  sizeof(wchar_t)));
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) {
            error = "写入注册表启动项失败";
            return false;
        }
        return true;
    }
    const LSTATUS status = RegDeleteValueW(key, ToWide(kAutoStartName).c_str());
    RegCloseKey(key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        error = "删除注册表启动项失败";
        return false;
    }
    return true;
#elif defined(__APPLE__)
    const std::string path = LaunchAgentPath();
    if (!enabled) {
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            error = "删除启动代理文件失败";
            return false;
        }
        return true;
    }
    // plist 所在目录可能不存在。
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !EnsureDirectory(path.substr(0, slash))) {
        error = "无法创建 LaunchAgents 目录";
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "无法写入启动代理文件";
        return false;
    }
    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         << "<plist version=\"1.0\">\n"
         << "<dict>\n"
         << "    <key>Label</key>\n"
         << "    <string>" << kAutoStartLabel << "</string>\n"
         << "    <key>ProgramArguments</key>\n"
         << "    <array>\n"
         << "        <string>" << exePath << "</string>\n"
         << "    </array>\n"
         << "    <key>RunAtLoad</key>\n"
         << "    <true/>\n"
         << "    <key>KeepAlive</key>\n"
         << "    <false/>\n"
         << "</dict>\n"
         << "</plist>\n";
    file.close();
    if (!file) {
        error = "写入启动代理文件失败";
        return false;
    }
    return true;
#else
    const std::string path = AutostartFilePath();
    if (!enabled) {
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            error = "删除自启动文件失败";
            return false;
        }
        return true;
    }
    if (!EnsureDirectory(AutostartDirectory())) {
        error = "无法创建 ~/.config/autostart 目录";
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "无法写入自启动文件";
        return false;
    }
    file << "[Desktop Entry]\n"
         << "Type=Application\n"
         << "Name=" << kAutoStartName << "\n"
         << "Exec=" << exePath << "\n"
         << "Terminal=false\n"
         << "X-GNOME-Autostart-enabled=true\n";
    file.close();
    if (!file) {
        error = "写入自启动文件失败";
        return false;
    }
    return true;
#endif
}

}  // namespace ppet
