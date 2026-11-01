// PPet - C++ 移植版
// 程序入口。
#include <cstdio>

#include "app/App.hpp"

int main(int argc, char** argv) {
    // 原 Electron 版在 Win7 上禁用硬件加速；此处保留一个通用的兼容开关位，
    // 具体是否需要由 GLFW/驱动自行判断。
    (void)argc;
    (void)argv;

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
