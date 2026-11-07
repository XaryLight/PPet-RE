// PPet - C++ 移植版
// 程序入口。
#include <cstdio>
#include <string_view>

#if defined(_WIN32)
// Windows 控制台默认使用本地代码页（简体中文系统是 GBK/936），
// 而源码里的中文是 UTF-8，直接 printf 会被当成 GBK 解释而显示成乱码
// （例如「尚未加载模型」显示成「灏氭湭鍔犺浇妯″瀷」）。
// 因此启动时把控制台输入/输出代码页切到 UTF-8。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <QApplication>

#include "app/App.hpp"
#include "core/Log.hpp"

namespace {

/// 命令行日志选项（在 Logger::Init 之后调用，优先级高于环境变量）：
///   --log-level=trace|debug|info|warn|error|off   文件与总体阈值
///   --log-console-level=...                       控制台阈值
///   --log-file=<路径>                             日志文件路径（需重新 Init）
/// 不消费参数，只是读取；QApplication 会把不认识的选项原样留在 argv 里，互不影响。
void ApplyLogOptions(int argc, char** argv) {
    constexpr std::string_view kLevel = "--log-level=";
    constexpr std::string_view kConsole = "--log-console-level=";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i] ? argv[i] : "");
        if (arg.starts_with(kLevel)) {
            ppet::Logger::SetLevel(
                ppet::ParseLogLevel(arg.substr(kLevel.size()), ppet::Logger::Level()));
        } else if (arg.starts_with(kConsole)) {
            ppet::Logger::SetConsoleLevel(ppet::ParseLogLevel(
                arg.substr(kConsole.size()), ppet::Logger::ConsoleLevel()));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // 原 Electron 版在 Win7 上禁用硬件加速；此处保留一个通用的兼容开关位，
    // 具体是否需要由 GLFW/驱动自行判断。

    // Qt 必须最先初始化（在任何 QWidget 创建之前）。设置窗口由 Qt 承载，
    // 关闭设置窗口不应退出整个程序（主窗口是 GLFW 窗口，不在 Qt 的管理范围内）。
    QApplication qtApp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);

#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // 调试日志落地到 ./last.log（UTF-8 + BOM，避免中文被当成 GBK 显示成乱码）。
    ppet::Logger::Init("last.log");
    ApplyLogOptions(argc, argv);

    PPET_LOG("PPet 启动 | C++ 标准=%d | 构建=%s %s | 日志=%s（级别 %s / 控制台 %s）",
             PPET_CXX_STANDARD, __DATE__, __TIME__, ppet::Logger::Path().c_str(),
             ppet::LogLevelName(ppet::Logger::Level()),
             ppet::LogLevelName(ppet::Logger::ConsoleLevel()));

    ppet::App app;

    std::string error;
    if (!app.Initialize(error)) {
        PPET_LOG_FATAL("启动失败：%s", error.c_str());
        return 1;
    }

    app.Run();
    app.Shutdown();

    PPET_LOG("PPet 正常退出");
    ppet::Logger::Shutdown();
    return 0;
}
