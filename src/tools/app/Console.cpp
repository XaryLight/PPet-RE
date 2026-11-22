// PPet - C++ 移植版
// 交互式控制台实现。
#include <src/tools/app/Console.hpp>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define PPET_ISATTY _isatty
#define PPET_FILENO _fileno
#else
#include <unistd.h>
#define PPET_ISATTY isatty
#define PPET_FILENO fileno
#endif

namespace ppet {
namespace {
// 缺陷 2.4.4：后台读线程的 prompt 与主线程 Print 都写 stdout，
// 无锁会交错成乱码。用进程级互斥串行化所有控制台输出。
std::mutex& OutputMutex() {
    static std::mutex m;
    return m;
}
/// 不带换行的原始输出（prompt 用），同样受 OutputMutex 保护。
/// 【2.4.1】prompt 现在只由主线程（Start/Poll）调用，读线程绝不触碰 stdout。
void WriteRaw(const char* text) {
    std::lock_guard<std::mutex> lock(OutputMutex());
    std::fputs(text, stdout);
    std::fflush(stdout);
}

/// 【2.4.1】从 stdin 读取一个原始字节，不经过任何 C++ iostream 全局对象。
/// 读线程只用 OS 级句柄：进程退出时 OS 回收 fd/handle，即使线程仍阻塞在
/// read 内被 exit() 强制终止，也不会访问已析构的 std::cin，消除 UB。
/// 逐字节读取让行边界处理变得平凡（遇 '\\n' 即成行），控制台输入量极小，
/// 性能影响可忽略。
/// @return 1 = 读到一字节（写入 *out）；0 = EOF；-1 = 错误/EINTR。
int ReadStdinByte(char* out) {
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return 0;
    DWORD got = 0;
    if (!ReadFile(h, out, 1, &got, nullptr)) return -1;
    return got == 0 ? 0 : 1;
#else
    const ssize_t n = ::read(STDIN_FILENO, out, 1);
    if (n > 0) return 1;
    if (n == 0) return 0;
    return (errno == EINTR) ? -1 : -2;  // -2 = 真实错误
#endif
}
}  // namespace

void Console::Print(const std::string& text) {
    std::lock_guard<std::mutex> lock(OutputMutex());
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

    // 【2.4.1】线程只捕获 shared_ptr<State>，不捕获 this，也绝不触碰任何
    // 全局流对象（std::cin / std::cout / stdout）：
    //   - 读 stdin 用 OS 级 read/ReadFile（见 ReadStdinByte）；
    //   - prompt 改由主线程打印。
    // 进程退出时即使线程仍阻塞在 read 内被 exit() 强制终止，也不会访问
    // 已析构的 C++ 流对象，消除 detach 线程的 UB。保留 detach 而非 join，
    // 避免 getline/read 无法唤醒导致的退出死锁。
    std::shared_ptr<State> state = state_;
    thread_ = std::thread([state]() {
        std::string line;
        line.reserve(256);
        for (;;) {
            if (state->stop.load()) break;
            char ch = 0;
            const int r = ReadStdinByte(&ch);
            if (r == 0) {  // EOF：把残留的半行也入队
                if (!line.empty()) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->pending.size() < State::kMaxPending) {
                        state->pending.push_back(line);
                    } else {
                        ++state->dropped;
                    }
                }
                break;
            }
            if (r == -1) continue;  // EINTR：重试
            if (r < 0) break;       // 真实错误：退出读线程
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (state->stop.load()) break;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->pending.size() < State::kMaxPending) {
                        state->pending.push_back(line);
                    } else {
                        ++state->dropped;
                    }
                }
                line.clear();
            } else {
                line.push_back(ch);
                // 防御：单行过长时截断并丢弃，避免无限增长吃内存。
                if (line.size() > State::kMaxLineBytes) line.clear();
            }
        }
        state->running.store(false);
    });

    // prompt 由主线程输出（读线程绝不碰 stdout）。
    if (interactive) WriteRaw(prompt_.c_str());
    Print("PPet 控制台已就绪，输入 help 查看指令。");
    return true;
}

void Console::Stop() {
    if (!state_) return;
    state_->stop.store(true);
    state_->running.store(false);
    if (thread_.joinable()) {
        // read/ReadFile 无法被可移植地唤醒，join 可能永远阻塞；detach 让进程
        // 退出时由 OS 回收。线程只持有 shared_ptr<State>，且只用 OS 级句柄
        // 读 stdin、不触碰任何全局流对象，因此被强制终止时不会 UB（见 2.4.1）。
        thread_.detach();
    }
    state_.reset();
}

void Console::SubmitLine(const std::string& line) {
    if (!state_) state_ = std::make_shared<State>();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->pending.size() < State::kMaxPending) {
        state_->pending.push_back(line);
    } else {
        ++state_->dropped;
    }
}

std::size_t Console::DroppedLines() const {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->dropped;
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
        if (cmd.empty) {
            if (interactive_) WriteRaw(prompt_.c_str());
            continue;
        }
        if (!cmd.ok) {
            Print("fail: " + cmd.error);
            if (interactive_) WriteRaw(prompt_.c_str());
            continue;
        }
        std::string result;
        try {
            result = handler_(cmd);
        } catch (const std::exception& e) {
            result = std::string("fail: 指令执行异常：") + e.what();
        }
        if (!result.empty()) Print(result);
        // 【2.4.1】prompt 由主线程打印（读线程不碰 stdout）。
        if (interactive_) WriteRaw(prompt_.c_str());
    }
}

}  // namespace ppet
