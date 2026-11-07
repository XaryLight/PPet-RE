// PPet - C++ 移植版
// 结构化日志：所有调试信息同时写到控制台（stderr）与 ./last.log。
//
// 【为什么需要它】
//   源码里的中文是 UTF-8，但 Windows 控制台 / 文件查看器默认按 GBK 解释，
//   直接 printf 会显示成乱码（如「尚未加载模型」变成「灏氭湭鍔犺浇妯″瀷」）。
//   日志统一写进 ./last.log，并在文件头写入 UTF-8 BOM，任何编辑器都会按 UTF-8 解析。
//
// 【日志精度（C++23 升级后）】
//   每一行都是自描述的、严格单行的、可机器解析的记录，字段用 " | " 分隔：
//
//   2026-11-07 12:34:56.789 | +0000123.456 | Δ   0.123 | INFO  | a3f1 | [PMX] PmxModel.cpp:209 PmxModel::Load | 模型已加载
//   └──────── 墙钟（毫秒）──┘ └─ 进程相对 ─┘ └ 距上条 ┘ └级别┘ └线程┘ └──────── 模块 / 源码位置 ────────┘ └ 消息
//
//   - 墙钟时间精确到毫秒（原实现只有 HH:MM:SS，无法定位先后顺序与卡顿）；
//   - 进程启动以来的毫秒数 + 距上一条日志的毫秒间隔，直接看出耗时台阶；
//   - std::source_location 自动带出「文件:行号 函数」，不用手写 __FILE__/__LINE__；
//   - 线程 ID 短哈希：Linux 托盘线程与主线程交错输出时能立刻区分；
//   - 模块标签自动从源码文件名推导，也可显式用 PPET_LOG_TAG 指定；
//   - 消息里的换行/制表符被转义，保证「一行一条」，grep / 日志采集不会串行；
//   - 6 级分级 + 文件/控制台双阈值，环境变量 PPET_LOG_LEVEL 动态调整；
//   - std::mutex 保护整行原子写出，多线程不会把两行搅在一起。
#pragma once

#include <cstdarg>
#include <source_location>
#include <string>
#include <string_view>

namespace ppet {

/// 日志级别（数值即优先级，越小越详细）。
enum class LogLevel : int {
    Trace = 0,  ///< 最细：循环内部、逐条目、逐帧状态（默认只进文件）
    Debug = 1,  ///< 调试：关键分支、参数快照
    Info  = 2,  ///< 常规：启动、加载、用户动作
    Warn  = 3,  ///< 可继续但数据/环境异常
    Error = 4,  ///< 功能失败
    Fatal = 5,  ///< 致命（不主动 abort，由调用方决定退出）
    Off   = 6,  ///< 全部关闭
};

/// 解析级别名（trace/debug/info/warn/warning/error/err/fatal/off，大小写不敏感）。
/// 无法识别时返回 fallback。供环境变量 PPET_LOG_LEVEL / PPET_LOG_CONSOLE_LEVEL 使用。
LogLevel ParseLogLevel(std::string_view text, LogLevel fallback);

/// 级别的定宽显示名（"INFO " 等，5 字符）。
const char* LogLevelName(LogLevel level);

/// 进程级日志单例（仅依赖标准库，不依赖任何图形库）。
class Logger {
public:
    /// 初始化：打开日志文件（默认 ./last.log，UTF-8 + BOM）。
    /// 传空串或 nullptr 表示沿用默认路径。文件打开失败时仅回退到控制台，不致命。
    ///
    /// 同时读取环境变量：
    ///   PPET_LOG_LEVEL         文件与总体阈值（默认 debug）
    ///   PPET_LOG_CONSOLE_LEVEL 控制台阈值（默认 info，避免 TRACE/DEBUG 刷屏）
    static void Init(const char* path = "last.log");

    /// 关闭文件。可重复调用；Init 可再次打开（测试会这么做）。
    static void Shutdown();

    /// 文件 / 总体阈值：低于此级别的日志直接丢弃（连字符串都不格式化）。
    static void SetLevel(LogLevel level);
    static LogLevel Level();

    /// 控制台阈值：低于此级别只写文件不打印（控制台默认 info）。
    static void SetConsoleLevel(LogLevel level);
    static LogLevel ConsoleLevel();

    /// 核心写出接口（由宏调用，自动带源码位置）。
    static void Write(LogLevel level, const std::source_location& loc,
                      const char* tag, const char* fmt, ...);

    /// va_list 版本。
    static void WriteV(LogLevel level, const std::source_location& loc,
                       const char* tag, const char* fmt, va_list args);

    /// 当前日志文件路径（初始化后可用于错误提示）。
    static const std::string& Path();

    /// 仅格式化成一行（不落盘、不打印），供单元测试与调试面板使用。
    static std::string Format(LogLevel level, const std::source_location& loc,
                              const char* tag, std::string_view message);
};

/// 变参包装：供宏调用（tag 为 nullptr 时自动从文件名推导）。
void Log_Write(LogLevel level, const std::source_location& loc,
               const char* tag, const char* fmt, ...);
void Log_WriteV(LogLevel level, const std::source_location& loc,
                const char* tag, const char* fmt, va_list args);

}  // namespace ppet

// ---------------------------------------------------------------------------
// 日志宏
//
// 兼容说明：老代码里大量写成 PPET_LOG("...\n", args)。新实现会自动去掉尾部换行、
// 把消息内部的换行转义成 "\\n"，因此这些调用不需要改动，输出仍是严格单行。
// ---------------------------------------------------------------------------
#define PPET_LOG_TRACE(...) \
    ::ppet::Log_Write(::ppet::LogLevel::Trace, ::std::source_location::current(), nullptr, __VA_ARGS__)
#define PPET_LOG_DEBUG(...) \
    ::ppet::Log_Write(::ppet::LogLevel::Debug, ::std::source_location::current(), nullptr, __VA_ARGS__)
#define PPET_LOG(...) \
    ::ppet::Log_Write(::ppet::LogLevel::Info, ::std::source_location::current(), nullptr, __VA_ARGS__)
#define PPET_LOG_WARN(...) \
    ::ppet::Log_Write(::ppet::LogLevel::Warn, ::std::source_location::current(), nullptr, __VA_ARGS__)
#define PPET_LOG_ERR(...) \
    ::ppet::Log_Write(::ppet::LogLevel::Error, ::std::source_location::current(), nullptr, __VA_ARGS__)
#define PPET_LOG_FATAL(...) \
    ::ppet::Log_Write(::ppet::LogLevel::Fatal, ::std::source_location::current(), nullptr, __VA_ARGS__)

/// 带显式模块标签的版本，例如 PPET_LOG_TAG("PMX", "贴图缺失：%s", name)。
#define PPET_LOG_TAG(tag, ...) \
    ::ppet::Log_Write(::ppet::LogLevel::Info, ::std::source_location::current(), tag, __VA_ARGS__)
#define PPET_LOG_TAG_DEBUG(tag, ...) \
    ::ppet::Log_Write(::ppet::LogLevel::Debug, ::std::source_location::current(), tag, __VA_ARGS__)
#define PPET_LOG_TAG_WARN(tag, ...) \
    ::ppet::Log_Write(::ppet::LogLevel::Warn, ::std::source_location::current(), tag, __VA_ARGS__)
#define PPET_LOG_TAG_ERR(tag, ...) \
    ::ppet::Log_Write(::ppet::LogLevel::Error, ::std::source_location::current(), tag, __VA_ARGS__)
