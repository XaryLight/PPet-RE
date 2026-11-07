// PPet - C++ 移植版
// 结构化日志实现：见 Log.hpp。
#include <src/tools/core/Log.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ppet {
namespace {

// ---------------------------------------------------------------------------
// 进程级状态（全部由 g_mutex 保护）
// ---------------------------------------------------------------------------
std::mutex g_mutex;
std::string g_path = "last.log";
FILE* g_file = nullptr;
LogLevel g_level = LogLevel::Debug;         // 文件 / 总体阈值
LogLevel g_consoleLevel = LogLevel::Info;   // 控制台阈值
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_last = g_start;

/// 单次写入的上限（超出部分截断，防止一条畸形日志打爆内存/磁盘）。
constexpr std::size_t kMessageBufferSize = 8192;
/// last.log 超过该大小就轮转成 last.log.1（Windows 中文路径下轮转失败会静默跳过）。
constexpr long kMaxLogBytes = 4 * 1024 * 1024;

// C++23 的 std::to_underlying：枚举转底层类型，不用再手写 static_cast<int>。
// 老标准库没有时退回 underlying_type_t，保证 GCC 11 也能编。
template <typename E>
constexpr auto Underlying(E value) noexcept {
#if defined(__cpp_lib_to_underlying)
    return std::to_underlying(value);
#else
    return static_cast<std::underlying_type_t<E>>(value);
#endif
}

/// 取路径的文件名部分（兼容 '/' 与 '\\'）。
std::string_view BaseName(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

/// 从 "PmxModel.cpp" 得到 "PmxModel"，用作自动模块标签。
std::string ModuleTag(const std::source_location& loc) {
    std::string name(BaseName(loc.file_name() ? loc.file_name() : "?"));
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.empty()) name = "PPet";
    return name;
}

/// 把编译器给出的函数签名精简成 "ppet::App::LoadModel"。
/// GCC/Clang 的 source_location::function_name() 形如
///   "bool ppet::App::LoadModel(std::string&, std::string&)"，
/// 去掉参数表与返回类型后正好是我们要的短名。
std::string ShortFunction(const char* raw) {
    std::string s(raw ? raw : "");
    if (const std::size_t paren = s.find('('); paren != std::string::npos) s.resize(paren);
    if (const std::size_t space = s.rfind(' '); space != std::string::npos) s.erase(0, space + 1);
    if (s.size() > 40) s.resize(40);
    if (s.empty()) s = "?";
    return s;
}

/// 消息规范化：严格单行。
///   1) 去掉尾部换行/空白（兼容老代码里 "xxx\n" 的写法）；
///   2) 内部换行转义成 "\\n"、制表符展开成空格、回车丢弃。
/// 这样一条日志一定占一行，grep / 日志采集不会串行。
std::string NormalizeMessage(std::string_view raw) {
    std::string s(raw);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
                          s.back() == '\t')) {
        s.pop_back();
    }
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "    "; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

/// 时间三件套：墙钟（毫秒）+ 进程相对毫秒 + 距上条日志毫秒。
struct ClockFields {
    char wall[32]{};
    char uptime[32]{};
    char delta[32]{};
};

ClockFields NowFields(bool advanceDelta) {
    using namespace std::chrono;
    const auto nowSteady = steady_clock::now();

    // --- 墙钟：YYYY-MM-DD HH:MM:SS.mmm ---
    const auto nowSystem = system_clock::now();
    const std::time_t epochSeconds = system_clock::to_time_t(nowSystem);
    const auto millis = duration_cast<milliseconds>(nowSystem.time_since_epoch()).count() % 1000;
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &epochSeconds);
#else
    localtime_r(&epochSeconds, &tmBuf);
#endif
    ClockFields f{};
    // 显式夹到合法范围：既防御异常的 tm，也让编译器确定 %02d/%04d 的输出宽度
    // （否则 GCC 会按「任意 int」推断出 -Wformat-truncation 误报）。
    const int year = std::clamp(tmBuf.tm_year + 1900, 0, 9999);
    const int month = std::clamp(tmBuf.tm_mon + 1, 1, 12);
    const int day = std::clamp(tmBuf.tm_mday, 1, 31);
    const int hour = std::clamp(tmBuf.tm_hour, 0, 23);
    const int minute = std::clamp(tmBuf.tm_min, 0, 59);
    const int second = std::clamp(tmBuf.tm_sec, 0, 60);
    const int ms = static_cast<int>(std::clamp<long long>(millis, 0, 999));
    std::snprintf(f.wall, sizeof(f.wall), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  year, month, day, hour, minute, second, ms);

    // --- 进程相对时间 ---
    const double uptimeMs = duration<double, std::milli>(nowSteady - g_start).count();
    std::snprintf(f.uptime, sizeof(f.uptime), "+%011.3f", uptimeMs);

    // --- 距上条日志的间隔（定位卡顿台阶）---
    const double deltaMs = duration<double, std::milli>(nowSteady - g_last).count();
    std::snprintf(f.delta, sizeof(f.delta), "%8.3f", deltaMs);
    if (advanceDelta) g_last = nowSteady;

    return f;
}

/// 线程 ID 短哈希：完整 std::thread::id 太长且不可读，取低 16 位足够区分。
const char* ThreadTag(char (&buf)[16]) {
    const std::size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(hash & 0xFFFFu));
    return buf;
}

std::string BuildLine(LogLevel level, const std::source_location& loc,
                      const char* tag, const std::string& message,
                      const ClockFields& clock) {
    char tidBuf[16];
    ThreadTag(tidBuf);

    const std::string module = tag && *tag ? std::string(tag) : ModuleTag(loc);
    const std::string fn = ShortFunction(loc.function_name());
    const std::string_view file = BaseName(loc.file_name() ? loc.file_name() : "?");
    const unsigned line = loc.line();

    char prefix[640];
    std::snprintf(prefix, sizeof(prefix),
                  "%s | %s | Δ%s | %s | %s | [%s] %.*s:%u %s | ",
                  clock.wall, clock.uptime, clock.delta, LogLevelName(level), tidBuf,
                  module.c_str(), static_cast<int>(file.size()), file.data(), line, fn.c_str());

    std::string result(prefix);
    result += message;
    result.push_back('\n');
    return result;
}

/// 超过上限时把 last.log 轮转成 last.log.1，然后重新打开一个空文件。
/// 返回是否需要写 BOM。
bool RotateIfNeeded(FILE*& file) {
    if (!file) return false;
    // "ab" 打开后初始位置由实现决定，必须先移到末尾才能拿到真实大小。
    std::fseek(file, 0, SEEK_END);
    const long current = std::ftell(file);
    if (current < kMaxLogBytes) return false;

    std::fclose(file);
    file = nullptr;
    const std::string backup = g_path + ".1";
    std::remove(backup.c_str());
    // Windows 窄字符 API 对中文路径可能失败；轮转只是优化，失败就继续追加，不致命。
    if (std::rename(g_path.c_str(), backup.c_str()) == 0) {
        file = std::fopen(g_path.c_str(), "wb");
        return file != nullptr;
    }
    file = std::fopen(g_path.c_str(), "ab");
    return false;
}

void WriteBom(FILE* file) {
    if (!file) return;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    std::fwrite(bom, 1, 3, file);
    std::fflush(file);
}

}  // namespace

// ---------------------------------------------------------------------------
// 级别
// ---------------------------------------------------------------------------

const char* LogLevelName(LogLevel level) {
    // 索引即 LogLevel 的数值；用 C++23 的 to_underlying 做下标，避免手写转换。
    static constexpr const char* kNames[] = {"TRACE", "DEBUG", "INFO ", "WARN ",
                                             "ERROR", "FATAL", "OFF  "};
    const auto index = Underlying(level);
    if (index < 0 || index >= static_cast<int>(std::size(kNames))) return "?????";
    return kNames[index];
}

LogLevel ParseLogLevel(std::string_view text, LogLevel fallback) {
    std::string s(text);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "trace" || s == "verbose") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info") return LogLevel::Info;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "error" || s == "err") return LogLevel::Error;
    if (s == "fatal") return LogLevel::Fatal;
    if (s == "off" || s == "none" || s == "silent") return LogLevel::Off;
    return fallback;
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

void Logger::Init(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (path && *path) g_path = path;
    g_start = std::chrono::steady_clock::now();
    g_last = g_start;

    // 环境变量优先级高于默认值；命令行 --log-level 会在 SetLevel 里再次覆盖。
    if (const char* env = std::getenv("PPET_LOG_LEVEL")) {
        g_level = ParseLogLevel(env, g_level);
    }
    if (const char* env = std::getenv("PPET_LOG_CONSOLE_LEVEL")) {
        g_consoleLevel = ParseLogLevel(env, g_consoleLevel);
    }

    g_file = std::fopen(g_path.c_str(), "ab");
    if (!g_file) {
        // 工作目录可能是只读的（例如安装目录），回退到系统临时目录。
        const char* tmp = std::getenv("TEMP");
        if (!tmp) tmp = std::getenv("TMP");
        if (!tmp) tmp = std::getenv("TMPDIR");
        if (tmp) {
            g_path = std::string(tmp) + "/ppet-last.log";
            g_file = std::fopen(g_path.c_str(), "ab");
        }
    }
    if (!g_file) return;

    if (RotateIfNeeded(g_file)) {
        WriteBom(g_file);
        return;
    }
    if (!g_file) return;
    std::fseek(g_file, 0, SEEK_END);
    if (std::ftell(g_file) == 0) WriteBom(g_file);
    std::fflush(g_file);
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}
LogLevel Logger::Level() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}
void Logger::SetConsoleLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_consoleLevel = level;
}
LogLevel Logger::ConsoleLevel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_consoleLevel;
}
const std::string& Logger::Path() { return g_path; }

std::string Logger::Format(LogLevel level, const std::source_location& loc,
                           const char* tag, std::string_view message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // 纯格式化：不推进 g_last，避免测试/预览影响真实日志的时间间隔统计。
    const ClockFields clock = NowFields(false);
    return BuildLine(level, loc, tag, NormalizeMessage(message), clock);
}

void Logger::Write(LogLevel level, const std::source_location& loc,
                   const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::WriteV(level, loc, tag, fmt, args);
    va_end(args);
}

void Logger::WriteV(LogLevel level, const std::source_location& loc,
                    const char* tag, const char* fmt, va_list args) {
    // 先在锁外做最便宜的判断，避免高频 TRACE 日志也抢锁。
    // （g_level/g_consoleLevel 是原子读写不上的普通变量，这里读取存在理论竞争，
    //   但只在 SetLevel 改变阈值的瞬间可能读到旧值，对日志无实质影响；
    //   真正的写出全程在锁内完成。）
    if (Underlying(level) >= Underlying(LogLevel::Off)) return;

    char raw[kMessageBufferSize];
    if (fmt) {
        std::vsnprintf(raw, sizeof(raw), fmt, args);
        raw[sizeof(raw) - 1] = '\0';
    } else {
        raw[0] = '\0';
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (Underlying(level) < Underlying(g_level)) return;  // 文件阈值

    const ClockFields clock = NowFields(true);
    const std::string line = BuildLine(level, loc, tag, NormalizeMessage(raw), clock);

    // 控制台：Windows 下 main 已 SetConsoleOutputCP(CP_UTF8)，UTF-8 字节可直接显示中文。
    if (Underlying(level) >= Underlying(g_consoleLevel)) {
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }
    // 文件：UTF-8（含 BOM），与控制台内容一致，但不再被当成 GBK 解释。
    if (g_file) {
        std::fputs(line.c_str(), g_file);
        std::fflush(g_file);
    }
}

// ---------------------------------------------------------------------------
// 宏用的包装
// ---------------------------------------------------------------------------

void Log_Write(LogLevel level, const std::source_location& loc,
               const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::WriteV(level, loc, tag, fmt, args);
    va_end(args);
}

void Log_WriteV(LogLevel level, const std::source_location& loc,
                const char* tag, const char* fmt, va_list args) {
    Logger::WriteV(level, loc, tag, fmt, args);
}

}  // namespace ppet
