// PPet - C++ 移植版
// 结构化日志单元测试（C++23 精度增强的回归保障）。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!condition) ++g_failures;
}

std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

/// 去掉 UTF-8 BOM。
std::string StripBom(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

bool Contains(const std::string& haystack, const std::string& needle) {
#if defined(__cpp_lib_string_contains)
    return haystack.contains(needle);
#else
    return haystack.find(needle) != std::string::npos;
#endif
}

/// 一条合格日志应包含的字段（顺序与 Log.hpp 注释一致）。
void CheckLineShape(const std::string& line, const std::string& context) {
    Check(line.find(" | ") != std::string::npos, context + "：使用 | 分隔字段");
    // 墙钟 YYYY-MM-DD HH:MM:SS.mmm
    const bool wallShape = line.size() > 23 && line[4] == '-' && line[7] == '-' &&
                           line[10] == ' ' && line[13] == ':' && line[16] == ':' &&
                           line[19] == '.';
    Check(wallShape, context + "：墙钟精确到毫秒");
    Check(line.find(" | +") != std::string::npos, context + "：含进程相对毫秒");
    Check(line.find("Δ") != std::string::npos, context + "：含距上条日志的间隔");
    Check(line.size() >= 3 && line.back() != '\n', context + "：单行（无尾随换行残留）");
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ppet_log_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string logPath = (dir / "test.log").string();

    std::printf("== 结构化日志单元测试 ==\n");

    // --- 1. 级别名与解析 ---
    Check(std::string(ppet::LogLevelName(ppet::LogLevel::Info)) == "INFO ", "级别名定宽");
    Check(ppet::ParseLogLevel("TRACE", ppet::LogLevel::Info) == ppet::LogLevel::Trace,
          "解析 trace（大小写不敏感）");
    Check(ppet::ParseLogLevel("warning", ppet::LogLevel::Info) == ppet::LogLevel::Warn,
          "解析 warning 别名");
    Check(ppet::ParseLogLevel("???", ppet::LogLevel::Warn) == ppet::LogLevel::Warn,
          "未知级别回退到 fallback");

    // --- 2. Format() 纯格式化，不落盘 ---
    {
        const auto loc = std::source_location::current();
        const std::string line = ppet::Logger::Format(ppet::LogLevel::Warn, loc, "TEST", "hello\nworld\t!");
        CheckLineShape(line.substr(0, line.size() - 1), "Format");
        Check(Contains(line, "WARN "), "Format：级别");
        Check(Contains(line, "[TEST]"), "Format：显式模块标签");
        Check(Contains(line, "log_test.cpp"), "Format：源码文件名");
        Check(Contains(line, "hello\\nworld    !"), "Format：消息内换行/制表符被转义");
        Check(line.find("\nworld") == std::string::npos, "Format：严格单行");
    }

    // --- 3. 写入文件 + 字段精度 ---
    ppet::Logger::Init(logPath.c_str());
    Check(ppet::Logger::Path() == logPath, "Init 使用指定路径");
    ppet::Logger::SetLevel(ppet::LogLevel::Trace);
    ppet::Logger::SetConsoleLevel(ppet::LogLevel::Off);  // 测试时保持 stdout 干净
    PPET_LOG("启动完成：%s", "中文 UTF-8");
    PPET_LOG_TAG("PMX", "贴图 %d 张", 3);
    PPET_LOG_TRACE("逐条目状态");
    PPET_LOG_DEBUG("调试快照");
    PPET_LOG_WARN("可疑：%s", "缺一张贴图");
    PPET_LOG_ERR("失败：%s", "文件不存在");
    ppet::Logger::Shutdown();

    const std::string content = StripBom(ReadAll(logPath));
    const std::vector<std::string> lines = SplitLines(content);
    Check(lines.size() == 6, "写入 6 条日志（实际 " + std::to_string(lines.size()) + "）");
    if (!lines.empty()) {
        CheckLineShape(lines[0], "落盘第 1 行");
        Check(Contains(lines[0], "INFO "), "落盘：INFO 级别");
        Check(Contains(lines[0], "中文 UTF-8"), "落盘：中文按 UTF-8 保留");
        Check(Contains(lines[0], "main"), "落盘：函数名");
        Check(Contains(lines[0], "log_test.cpp:"), "落盘：源码行号");
    }
    Check(content.find("[PMX]") != std::string::npos, "落盘：显式 tag");
    Check(content.find("TRACE") != std::string::npos, "落盘：TRACE 级别");
    Check(content.find("ERROR") != std::string::npos, "落盘：ERROR 级别");
    // 老代码习惯：fmt 里带 \n，不应产生空行
    Check(content.find("\n\n") == std::string::npos, "落盘：尾部 \\n 被去掉，无空行");

    // --- 4. 级别阈值过滤 ---
    const std::string filteredPath = (dir / "filtered.log").string();
    ppet::Logger::Init(filteredPath.c_str());
    ppet::Logger::SetConsoleLevel(ppet::LogLevel::Off);
    ppet::Logger::SetLevel(ppet::LogLevel::Warn);
    PPET_LOG_TRACE("应被丢弃 trace");
    PPET_LOG_DEBUG("应被丢弃 debug");
    PPET_LOG("应被丢弃 info");
    PPET_LOG_WARN("保留 warn");
    PPET_LOG_ERR("保留 error");
    ppet::Logger::Shutdown();
    const std::string filtered = ReadAll(filteredPath);
    Check(filtered.find("应被丢弃") == std::string::npos, "阈值：低于 Warn 全部丢弃");
    Check(Contains(filtered, "保留 warn") && Contains(filtered, "保留 error"),
          "阈值：Warn/Error 保留");
    Check(SplitLines(StripBom(filtered)).size() == 2, "阈值：恰好 2 行");

    // --- 5. 自动模块标签 ---
    const std::string tagPath = (dir / "tag.log").string();
    ppet::Logger::Init(tagPath.c_str());
    ppet::Logger::SetConsoleLevel(ppet::LogLevel::Off);
    ppet::Logger::SetLevel(ppet::LogLevel::Info);
    PPET_LOG("无显式 tag");
    ppet::Logger::Shutdown();
    Check(Contains(ReadAll(tagPath), "[log_test]"), "模块标签自动从文件名推导");

    // --- 6. 多线程：整行原子写出，不交错 ---
    const std::string mtPath = (dir / "mt.log").string();
    ppet::Logger::Init(mtPath.c_str());
    ppet::Logger::SetConsoleLevel(ppet::LogLevel::Off);
    ppet::Logger::SetLevel(ppet::LogLevel::Trace);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 40;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kPerThread; ++i) {
                PPET_LOG_TRACE("线程 %d 第 %d 条 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();
    ppet::Logger::Shutdown();
    const auto mtLines = SplitLines(StripBom(ReadAll(mtPath)));
    Check(static_cast<int>(mtLines.size()) == kThreads * kPerThread,
          "多线程：行数完整（" + std::to_string(mtLines.size()) + "）");
    bool allShaped = true;
    for (const auto& line : mtLines) {
        if (line.find("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == std::string::npos ||
            line.find("TRACE") == std::string::npos) {
            allShaped = false;
            break;
        }
    }
    Check(allShaped, "多线程：每一条都完整且未被其它线程插入");

    fs::remove_all(dir, ec);
    std::printf("== 结果：%s（失败 %d 项）==\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
