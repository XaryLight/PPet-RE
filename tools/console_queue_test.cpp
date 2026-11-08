// PPet - C++ 移植版
// 控制台命令队列的长期运行边界：有界队列、丢弃计数、每帧消费上限。
#include <src/tools/app/Console.hpp>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    std::printf("== 控制台队列稳定性单测 ==\n");

    ppet::Console console;
    console.SetEcho(false);
    int executed = 0;
    console.SetHandler([&](const ppet::console::Command&) {
        ++executed;
        return std::string();  // 返回空串，避免测试刷屏
    });

    // 灌入远超队列上限的命令（模拟脚本注入 / 主循环卡帧时 stdin 继续写入）。
    constexpr int kInjected = 5000;
    for (int i = 0; i < kInjected; ++i) console.SubmitLine("ping");

    const std::size_t dropped = console.DroppedLines();
    Check(dropped > 0, "超过上限的命令被丢弃并计数");
    Check(dropped == static_cast<std::size_t>(kInjected - 4096),
          "丢弃数精确等于超出上限的条数（队列有界）");

    // 每帧最多消费 32 条，避免一帧被超长脚本卡住。
    console.Poll();
    Check(executed == 32, "单帧最多执行 32 条命令");

    // 剩余命令在后续若干帧内被消费完，且总数就是队列上限。
    for (int i = 0; i < 200; ++i) console.Poll();
    Check(executed == 4096, "队列内容最终全部被消费，没有无限堆积");

    std::printf("== 结果：%s（失败 %d 项）==\n", g_failures == 0 ? "全部通过" : "存在失败",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
