// PPet - C++ 移植版
// 设置界面（Qt 6 独立窗口）冒烟测试。
//
// 验证点：
//   1. QApplication 已创建，能构造 QWidget；
//   2. SettingsPanel::Initialize 成功，Handle() 非空；
//   3. 初始为隐藏状态；Show() 后可见，Hide() 后隐藏；
//   4. PumpEvents() / Refresh() 连续调用不崩溃；
//   5. Toggle() 可见性切换正确。
//
// 使用 offscreen 平台插件运行（见 CMake 中的 QT_QPA_PLATFORM=offscreen），
// 无需真实显示器；offscreen 插件不可用或 QApplication 初始化失败时以退出码 2 跳过。
#include <cstdio>
#include <string>

#include <QApplication>

#include <src/tools/core/Config.hpp>
#include <src/tools/core/I18n.hpp>
#include <src/ui/SettingsPanel.hpp>

int main(int argc, char** argv) {
    namespace ppet = ppet;

    // Qt 必须在任何 QWidget 之前初始化。
    QApplication qtApp(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    if (!QApplication::instance()) {
        std::printf("settings_panel_smoke: QApplication 初始化失败（offscreen 插件可用？）\n");
        return 2;
    }

    // 模拟配置与文案。
    ppet::AppConfig cfg;
    ppet::Strings strings = ppet::GetStrings(ppet::Language::Chinese);
    cfg.modelPath = "C:/models/demo.vrm";
    cfg.modelList.push_back("C:/models/demo.vrm");
    cfg.modelList.push_back("C:/models/demo.pmx");
    cfg.fullscreenTracking = true;
    cfg.trackingRadius = 400.0f;
    cfg.maxTrackingAngle = 30.0f;

    int importCalls = 0;
    int removeCalls = 0;
    int selectCalls = 0;
    int changeCalls = 0;

    ppet::SettingsPanel panel;
    panel.config = &cfg;
    panel.strings = &strings;
    panel.onImportModel = [&importCalls](const std::string&) { ++importCalls; };
    panel.onRemoveModel = [&removeCalls](int) { ++removeCalls; };
    panel.onSelectModel = [&selectCalls](int) { ++selectCalls; };
    panel.onConfigChanged = [&changeCalls]() { ++changeCalls; };

    std::string error;
    if (!panel.Initialize(error)) {
        std::printf("settings_panel_smoke: 设置面板初始化失败：%s\n", error.c_str());
        return 1;
    }

    if (panel.Handle() == nullptr) {
        std::printf("settings_panel_smoke: 设置窗口句柄为空\n");
        panel.Shutdown();
        return 1;
    }

    // 初始应为隐藏状态。
    if (panel.IsVisible()) {
        std::printf("settings_panel_smoke: 初始化后不应可见\n");
        panel.Shutdown();
        return 1;
    }

    // 显示并驱动若干帧 Qt 事件。
    panel.Show();
    if (!panel.IsVisible()) {
        std::printf("settings_panel_smoke: Show() 后应可见\n");
        panel.Shutdown();
        return 1;
    }

    for (int i = 0; i < 10; ++i) {
        panel.PumpEvents();
    }

    // 刷新不应崩溃（模拟语言切换 / 清除设置后的文案与数值重绘）。
    strings = ppet::GetStrings(ppet::Language::English);
    panel.strings = &strings;
    panel.Refresh();
    panel.PumpEvents();

    // 可见性切换。
    panel.Toggle();
    if (panel.IsVisible()) {
        std::printf("settings_panel_smoke: Toggle() 后应隐藏\n");
        panel.Shutdown();
        return 1;
    }
    panel.Toggle();
    if (!panel.IsVisible()) {
        std::printf("settings_panel_smoke: 再次 Toggle() 后应可见\n");
        panel.Shutdown();
        return 1;
    }

    panel.Hide();
    if (panel.IsVisible()) {
        std::printf("settings_panel_smoke: Hide() 后应隐藏\n");
        panel.Shutdown();
        return 1;
    }

    panel.Shutdown();

    std::printf("settings_panel_smoke: 通过（import=%d remove=%d select=%d change=%d）\n",
                importCalls, removeCalls, selectCalls, changeCalls);
    return 0;
}
