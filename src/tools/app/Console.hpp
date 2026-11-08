// PPet - C++ 移植版
// 交互式控制台：后台线程读 stdin，主线程执行命令。
//
// 【为什么用后台线程】
//   主循环每帧不能阻塞在 std::getline 上，否则窗口会卡死。托盘层已有
//   「独立线程 → 队列 → 主循环 PumpEvents」的先例，这里沿用同一模式。
//   命令的**执行**必须在主线程（要访问 model_ / configStore_ / Qt 气泡），
//   所以线程只负责读字符串并入队，Poll() 在主循环里取出来执行。
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <src/tools/app/ConsoleCommand.hpp>

namespace ppet {

class Console {
public:
    /// 执行器：返回要打印给用户的结果文本（可多行）。
    using Handler = std::function<std::string(const console::Command&)>;

    Console() = default;
    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void SetHandler(Handler handler) { handler_ = std::move(handler); }
    void SetPrompt(const std::string& prompt) { prompt_ = prompt; }
    /// 脚本模式下关掉命令回显，输出更干净。
    void SetEcho(bool echo) { echo_ = echo; }

    /// 启动 stdin 读取线程。
    /// @param force true = 即使 stdin 不是终端也启动（--console）。
    /// @return 是否真的启动了线程。没有终端且未 force 时返回 false，
    ///         此时仍可用 SubmitLine() + Poll() 走脚本化模式。
    bool Start(bool force = false);

    /// 停止读取线程。线程可能正阻塞在 getline 上，因此用共享状态 + detach，
    /// 保证不会因 join 而卡住程序退出，也不会访问已析构的 Console 对象。
    void Stop();

    /// 主线程每帧调用：取出待执行命令、执行并打印结果。
    void Poll();

    /// 注入一行命令（脚本模式 / 单元测试用，线程安全）。
    void SubmitLine(const std::string& line);

    /// 一次性注入多行（用 ';' 分隔）。
    void SubmitScript(const std::string& script);

    bool Enabled() const { return state_ != nullptr && state_->running.load(); }

    /// 是否以「交互式 stdin 读取」模式启动（--console 强制，或 stdin 本身是终端）。
    /// 与 Enabled() 不同：管道输入很快就 EOF、读线程会退出，但用户确实是
    /// 开着交互式控制台的，不能用「线程还活着」来判断模式。
    bool Interactive() const { return interactive_; }

    /// 【长期运行】被队列上限丢弃的命令条数。
    /// 后台 stdin 线程可能比主循环消费得快（脚本灌入 / 卡帧），若队列无界增长，
    /// 跑几天就会吃掉大量内存。超过 kMaxPending 时丢弃新行并在此计数。
    std::size_t DroppedLines() const;

    /// 打印一行到 stdout（与日志 stderr 分离）。
    static void Print(const std::string& text);

private:
    struct State {
        /// 待执行命令队列上限。
        static constexpr std::size_t kMaxPending = 4096;
        std::mutex mutex;
        std::deque<std::string> pending;
        /// 由 mutex 保护（与 pending 同一临界区）。
        std::size_t dropped = 0;
        std::atomic<bool> running{false};
        std::atomic<bool> stop{false};
    };

    Handler handler_;
    std::shared_ptr<State> state_;
    std::thread thread_;
    std::string prompt_ = "> ";
    bool echo_ = true;
    bool interactive_ = false;
};

}  // namespace ppet
