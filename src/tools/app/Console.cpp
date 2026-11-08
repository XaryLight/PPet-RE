// PPet - C++ 移植版
// 交互式控制台实现。
#include <src/tools/app/Console.hpp>

#include <cstdio>
#include <exception>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#include <io.h>
#define PPET_ISATTY _isatty
#define PPET_FILENO _fileno
#else
#include <unistd.h>
#define PPET_ISATTY isatty
#define PPET_FILENO fileno
#endif

namespace ppet {

void Console::Print(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

Console::~Console() { Stop(); }

bool Console::Start(bool force) {
    if (state_ && state_->running.load()) return true;
    const bool interactive = PPET_ISATTY(PPET_FILENO(stdin)) != 0;
    if (!interactive && !force) return false;

    // 复用已有 State：SubmitScript() 可能已经创建了 state_ 并塞进了脚本命令。
    // 直接 make_shared 覆盖会丢掉整批 --command 指令（同时传 --console 与
    // --command 时必现），因此只在还没有 state_ 时才创建。
    if (!state_) state_ = std::make_shared<State>();
    interactive_ = true;
    state_->running.store(true);
    state_->stop.store(false);

    // 线程只捕获 shared_ptr<State>，不捕获 this：
    // Stop() 之后即使线程还阻塞在 getline，也不会碰到已析构的 Console。
    std::shared_ptr<State> state = state_;
    const std::string prompt = interactive ? prompt_ : std::string();
    thread_ = std::thread([state, prompt]() {
        std::string line;
        for (;;) {
            if (state->stop.load()) break;
            if (!prompt.empty()) {
                std::fputs(prompt.c_str(), stdout);
                std::fflush(stdout);
            }
            if (!std::getline(std::cin, line)) break;  // EOF / stdin 被关闭
            if (state->stop.load()) break;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->pending.push_back(line);
            }
        }
        state->running.store(false);
    });

    if (!interactive) {
        // 非交互强制模式：读线程没有意义（没有 prompt，也很快 EOF），
        // 但保留它以便管道输入也能被消费。
    }
    Print("PPet 控制台已就绪，输入 help 查看指令。");
    return true;
}

void Console::Stop() {
    if (!state_) return;
    state_->stop.store(true);
    state_->running.store(false);
    if (thread_.joinable()) {
        // getline 无法被唤醒，join 可能永远阻塞；detach 让进程退出时自然回收。
        // 线程只持有 shared_ptr<State>，不会访问已析构的成员。
        thread_.detach();
    }
    state_.reset();
}

void Console::SubmitLine(const std::string& line) {
    if (!state_) state_ = std::make_shared<State>();
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->pending.push_back(line);
}

void Console::SubmitScript(const std::string& script) {
    std::istringstream is(script);
    std::string line;
    while (std::getline(is, line, ';')) {
        // 去掉首尾空白。
        const std::size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const std::size_t last = line.find_last_not_of(" \t\r\n");
        SubmitLine(line.substr(first, last - first + 1));
    }
}

void Console::Poll() {
    if (!state_ || !handler_) return;

    // 一次取空队列，避免命令堆积；但每帧最多处理 32 条，防止脚本过长卡住一帧。
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        while (!state_->pending.empty() && lines.size() < 32) {
            lines.push_back(std::move(state_->pending.front()));
            state_->pending.pop_front();
        }
    }

    for (const std::string& line : lines) {
        if (echo_) Print(prompt_ + line);
        const console::Command cmd = console::ParseCommand(line);
        if (cmd.empty) continue;
        if (!cmd.ok) {
            Print("fail: " + cmd.error);
            continue;
        }
        std::string result;
        try {
            result = handler_(cmd);
        } catch (const std::exception& e) {
            result = std::string("fail: 指令执行异常：") + e.what();
        }
        if (!result.empty()) Print(result);
    }
}

}  // namespace ppet
