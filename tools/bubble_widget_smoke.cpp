// PPet - C++ 移植版
// 台词气泡窗口（Qt 6 独立窗口）冒烟测试。
//
// 验证点：
//   1. Initialize 在有 QApplication 时成功，重复调用幂等；
//   2. SetText("") 隐藏气泡；SetText(非空) 显示并保留文本；
//   3. MoveTo 只移动位置，不改变可见性；
//   4. SetVisible(true/false) 正确切换显隐；
//   5. Text() 回读与设置一致；
//   6. Shutdown 可重复调用，之后再 SetText 不崩。
//
// 用 offscreen 平台插件运行（见 CMake 中的 QT_QPA_PLATFORM=offscreen）。
#include <cstdio>
#include <string>

#include <QApplication>
#include <QLabel>
#include <QWidget>

#include <src/ui/BubbleWidget.hpp>

int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    if (!QApplication::instance()) {
        std::printf("bubble_widget_smoke: QApplication 初始化失败（offscreen 插件可用？）\n");
        return 2;
    }

    ppet::BubbleWidget bubble;

    std::string error;
    if (!bubble.Initialize(error)) {
        std::printf("bubble_widget_smoke: 初始化失败：%s\n", error.c_str());
        return 1;
    }
    // 幂等：重复初始化不应失败，也不应重建窗口。
    if (!bubble.Initialize(error)) {
        std::printf("bubble_widget_smoke: 重复初始化失败：%s\n", error.c_str());
        return 1;
    }

    // 初始状态：没有文本 => 不可见。
    if (bubble.IsVisible()) {
        std::printf("bubble_widget_smoke: 初始化后不应可见\n");
        return 1;
    }
    if (!bubble.Text().empty()) {
        std::printf("bubble_widget_smoke: 初始文本应为空\n");
        return 1;
    }

    // SetText 只改文本，不改显隐（显隐由 SetVisible 单独控制）。
    const std::string phrase = "今天也要加油呀～";
    bubble.SetText(phrase);
    if (bubble.IsVisible()) {
        std::printf("bubble_widget_smoke: SetText 不应改变可见性\n");
        return 1;
    }
    if (bubble.Text() != phrase) {
        std::printf("bubble_widget_smoke: 文本回读不一致：%s\n", bubble.Text().c_str());
        return 1;
    }

    // 显隐开关：打开后应可见。
    bubble.SetVisible(true);
    if (!bubble.IsVisible()) {
        std::printf("bubble_widget_smoke: SetVisible(true) 后应可见\n");
        return 1;
    }

    // MoveTo 只改位置，不改可见性。
    bubble.MoveTo(120, 240);
    if (!bubble.IsVisible()) {
        std::printf("bubble_widget_smoke: MoveTo 后不应改变可见性\n");
        return 1;
    }
    if (auto* label = bubble.Handle() ? bubble.Handle()->findChild<QLabel*>()
                                      : nullptr) {
        if (label->text().toStdString() != phrase) {
            std::printf("bubble_widget_smoke: QLabel 文本不一致\n");
            return 1;
        }
    } else {
        std::printf("bubble_widget_smoke: 未找到气泡 QLabel\n");
        return 1;
    }

    // 清空文本：文本消失，但显隐仍由 SetVisible 决定。
    bubble.SetText("");
    if (!bubble.Text().empty()) {
        std::printf("bubble_widget_smoke: 清空后文本应为空\n");
        return 1;
    }

    // 显隐开关（反向）。
    bubble.SetVisible(false);
    if (bubble.IsVisible()) {
        std::printf("bubble_widget_smoke: SetVisible(false) 后不应可见\n");
        return 1;
    }
    bubble.SetVisible(true);
    if (!bubble.IsVisible()) {
        std::printf("bubble_widget_smoke: SetVisible(true) 后应可见\n");
        return 1;
    }

    // Shutdown 可重复调用；之后调用 SetText 不应崩。
    bubble.Shutdown();
    bubble.Shutdown();
    bubble.SetText("关闭之后也不应崩");
    bubble.MoveTo(0, 0);

    std::printf("bubble_widget_smoke: 通过\n");
    return 0;
}
