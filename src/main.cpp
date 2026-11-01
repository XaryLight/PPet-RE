// PPet - C++ 移植版
// 程序入口。
#include <cstdio>

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

#include "app/App.hpp"

int main(int argc, char** argv) {
    // 原 Electron 版在 Win7 上禁用硬件加速；此处保留一个通用的兼容开关位，
    // 具体是否需要由 GLFW/驱动自行判断。
    (void)argc;
    (void)argv;

#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    ppet::App app;

    std::string error;
    if (!app.Initialize(error)) {
        std::fprintf(stderr, "[PPet] 启动失败：%s\n", error.c_str());
        return 1;
    }

    app.Run();
    app.Shutdown();
    return 0;
}
