// PPet - C++ 移植版
// 轻量日志：所有 [PPet] 调试信息同时写到控制台（stderr）与 ./last.log。
//
// 为什么需要它：
//   源码里的中文是 UTF-8，但 Windows 控制台 / 文件查看器默认按 GBK 解释，
//   直接 printf 会显示成乱码（如「尚未加载模型」变成「灏氭湭鍔犺浇妯″瀷」）。
//   这里把日志统一写进 ./last.log，并在文件头写入 UTF-8 BOM，
//   任何编辑器都会按 UTF-8 解析，乱码问题彻底消失。
#pragma once

#include <cstdarg>
#include <string>

namespace ppet {

/// 进程级日志单例（仅用到 stdio / cstdlib，不依赖任何图形库）。
class Logger {
public:
    /// 初始化：打开日志文件（默认 ./last.log，UTF-8 + BOM）。
    /// 文件打开失败时仅回退到控制台输出，不致命。
    static void Init(const char* path = "last.log");

    /// 关闭文件。
    static void Shutdown();

    /// 写出一行（已带 [PPet][LEVEL][时间] 前缀），同时落控制台与文件。
    static void Write(const char* level, const char* fmt, va_list args);

    /// 当前日志文件路径（初始化后可用于错误提示）。
    static const std::string& Path() { return path_; }

private:
    static std::string path_;
    static FILE* file_;
};

/// 变参包装：供宏调用。
void Log_Write(const char* level, const char* fmt, ...);

}  // namespace ppet

/// 普通信息日志（自动加 [PPet] 前缀，UTF-8，同时写 ./last.log）。
#define PPET_LOG(...) ppet::Log_Write("INFO", __VA_ARGS__)

/// 警告日志（同上，LEVEL 标记为 WARN）。
/// 用于「还能继续跑，但数据/环境有点不对劲」这类情况。
#define PPET_LOG_WARN(...) ppet::Log_Write("WARN", __VA_ARGS__)

/// 错误日志（同上，LEVEL 标记为 ERR）。
#define PPET_LOG_ERR(...) ppet::Log_Write("ERR ", __VA_ARGS__)
