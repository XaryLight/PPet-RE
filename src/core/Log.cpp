// PPet - C++ 移植版
// 日志实现：见 Log.hpp。
#include "core/Log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace ppet {

std::string Logger::path_ = "last.log";
FILE* Logger::file_ = nullptr;

void Logger::Init(const char* path) {
    if (path && *path) path_ = path;

    // 追加模式打开；首次（文件为空）写入 UTF-8 BOM，保证编辑器按 UTF-8 解析。
    file_ = std::fopen(path_.c_str(), "ab");
    if (!file_) {
        // 工作目录可能是只读的（例如安装目录），回退到系统临时目录。
        const char* tmp = std::getenv("TEMP");
        if (!tmp) tmp = std::getenv("TMP");
        if (!tmp) tmp = std::getenv("TMPDIR");
        if (tmp) {
            std::string p = std::string(tmp) + "/ppet-last.log";
            path_ = p;
            file_ = std::fopen(path_.c_str(), "ab");
        }
    }
    if (file_) {
        std::fseek(file_, 0, SEEK_END);
        if (std::ftell(file_) == 0) {
            const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
            std::fwrite(bom, 1, 3, file_);
        }
        std::fflush(file_);
    }
}

void Logger::Shutdown() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Logger::Write(const char* level, const char* fmt, va_list args) {
    // 时间前缀 HH:MM:SS
    char timeBuf[32] = {0};
    const time_t now = std::time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf);

    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);

    char line[2304];
    const int n = snprintf(line, sizeof(line), "[PPet][%s][%s] %s\n", level, timeBuf, msg);
    (void)n;

    // 控制台：Windows 下 main 已 SetConsoleOutputCP(CP_UTF8)，UTF-8 字节可直接显示中文。
    std::fputs(line, stderr);

    // 文件：UTF-8（含 BOM），与控制台内容一致，但不再被当成 GBK 解释。
    if (file_) {
        std::fputs(line, file_);
        std::fflush(file_);
    }
}

void Log_Write(const char* level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::Write(level, fmt, args);
    va_end(args);
}

}  // namespace ppet
