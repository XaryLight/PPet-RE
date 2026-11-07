// PPet - C++ 移植版
// 文件选择对话框：平台实现。
#include <src/tools/platform/FileDialog.hpp>

#include <cstdio>
#include <cstring>
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
#include <commdlg.h>

#elif defined(__APPLE__)
#import <AppKit/AppKit.h>

#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace ppet {
namespace {

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

/// 把 "*.pmx;*.vrm" 转换成 Win32 过滤器需要的分号分隔形式（已经是了）。
std::wstring BuildWin32Filter(const std::string& filterName, const std::string& patterns) {
    std::wstring filter;
    filter += ToWide(filterName);
    filter += L'\0';
    filter += ToWide(patterns);
    filter += L'\0';
    filter += L"所有文件";
    filter += L'\0';
    filter += L"*.*";
    filter += L'\0';
    filter += L'\0';
    return filter;
}

#elif defined(__APPLE__)

/// UTF-8 → NSString。
NSString* ToNSString(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()];
}

/// 把 "*.pmx;*.vrm" 转换成 NSOpenPanel 需要的扩展名列表（不含点号）。
std::vector<std::string> ParseExtensions(const std::string& patterns) {
    std::vector<std::string> extensions;
    std::string current;
    for (char c : patterns) {
        if (c == ';' || c == ',' || c == ' ') {
            if (!current.empty()) {
                extensions.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) extensions.push_back(current);

    std::vector<std::string> result;
    for (const std::string& item : extensions) {
        std::string extension = item;
        if (!extension.empty() && extension[0] == '*') extension.erase(0, 1);
        if (!extension.empty() && extension[0] == '.') extension.erase(0, 1);
        if (!extension.empty()) result.push_back(extension);
    }
    return result;
}

#else

/// 执行命令并收集标准输出。
/// @return 退出码；找不到命令时返回 -1
int RunAndCapture(const std::vector<std::string>& arguments, std::string& output) {
    output.clear();
    int pipeFds[2] = {-1, -1};
    if (pipe(pipeFds) != 0) return -1;

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return -1;
    }
    if (pid == 0) {
        // 子进程：把标准输出接到管道上。
        close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[1]);

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipeFds[1]);
    char buffer[512];
    ssize_t count = 0;
    while ((count = read(pipeFds[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(pipeFds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

/// 去掉行尾的换行与空白。
std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
                           text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
                           text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

#endif

}  // namespace

bool OpenFileDialog(const std::string& title, const std::string& filterName,
                    const std::string& patterns, const std::string& defaultPath,
                    void* nativeOwner, std::string& outPath, std::string& error) {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(4096, L'\0');
    const std::wstring filter = BuildWin32Filter(filterName, patterns);
    // 标题与初始目录必须保存为具名对象：直接把临时 std::wstring 的 c_str()
    // 赋给结构体成员会留下悬垂指针（临时量在语句结束时就已析构）。
    const std::wstring dialogTitle = ToWide(title);
    const std::wstring initialDir = defaultPath.empty() ? std::wstring() : ToWide(defaultPath);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nativeOwner ? static_cast<HWND>(nativeOwner) : nullptr;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = dialogTitle.c_str();
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR |
                OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) {
        const DWORD code = CommDlgExtendedError();
        if (code == 0) {
            // 用户直接取消，不算错误。
            return false;
        }
        error = "打开文件对话框失败（错误码 " + std::to_string(code) + "）";
        return false;
    }
    outPath = FromWide(std::wstring(buffer.data()));
    return true;
#elif defined(__APPLE__)
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setTitle:ToNSString(title)];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    [panel setAllowsMultipleSelection:NO];

    const std::vector<std::string> extensions = ParseExtensions(patterns);
    if (!extensions.empty()) {
        NSMutableArray<NSString*>* types = [NSMutableArray array];
        for (const std::string& extension : extensions) {
            [types addObject:[NSString stringWithUTF8String:extension.c_str()]];
        }
        [panel setAllowedFileTypes:types];
    }
    if (!defaultPath.empty()) {
        [panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:defaultPath.c_str()]]];
    }
    if (nativeOwner) {
        [panel setCanCreateDirectories:YES];
    }

    if ([panel runModal] != NSModalResponseOK) {
        return false;  // 用户取消
    }
    NSURL* url = [panel URL];
    if (!url) {
        error = "未获取到所选文件";
        return false;
    }
    outPath = std::string([[url path] UTF8String]);
    return true;
#else
    // Linux：优先使用 zenity，其次 kdialog。
    std::string output;
    std::vector<std::string> zenityArguments = {"zenity", "--file-selection", "--file-filter",
                                                filterName + " | " + patterns};
    if (!defaultPath.empty()) {
        zenityArguments.push_back("--filename=" + defaultPath + "/");
    }
    int code = RunAndCapture(zenityArguments, output);
    if (code == 0) {
        const std::string selected = Trim(output);
        if (selected.empty()) return false;
        outPath = selected;
        return true;
    }
    if (code == 1) {
        // zenity 返回 1 表示用户取消。
        return false;
    }

    std::vector<std::string> kdialogArguments = {"kdialog", "--title", title,
                                                 "--getopenfilename",
                                                 defaultPath.empty() ? "." : defaultPath};
    if (!patterns.empty()) {
        kdialogArguments.push_back(patterns + "|" + filterName);
    }
    code = RunAndCapture(kdialogArguments, output);
    if (code == 0) {
        const std::string selected = Trim(output);
        if (selected.empty()) return false;
        outPath = selected;
        return true;
    }
    if (code == 1) {
        return false;  // 用户取消
    }

    // zenity / kdialog 都是独立进程，没有「父窗口」的概念，
    // 因此 X11 下无法把对话框挂到本程序窗口上，这里忽略 nativeOwner。
    (void)nativeOwner;

    error = "未找到可用的文件对话框程序（zenity / kdialog），请改用路径输入";
    return false;
#endif
}

}  // namespace ppet
