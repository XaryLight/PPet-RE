// PPet - C++ 移植版
// 台词气泡窗口：跟随桌宠显示当前台词的小气泡。
//
// 【为什么单独一个 Qt 窗口】
//   桌宠主窗口是 GLFW 透明叠加窗口；台词气泡是临时性的 UI 提示，用 Qt 画更省事。
//   它默认「不置顶」——遵循「软件置顶只作用渲染窗口，不把 Qt GUI 一起顶到最前」
//   的既有约定（设置/诊断面板同理不抢置顶），避免盖住用户正在操作的其他窗口。
//
// 【线程与生命周期】
//   与 SettingsPanel / DebugPanel 一样：QApplication 已在主线程创建，
//   App 每帧调用 PumpEvents() 驱动事件；本类不是 QObject，无需 moc/AUTOMOC。
#pragma once

#include <string>

class QWidget;
class QLabel;

namespace ppet {

class BubbleWidget {
public:
    BubbleWidget();
    ~BubbleWidget();

    BubbleWidget(const BubbleWidget&) = delete;
    BubbleWidget& operator=(const BubbleWidget&) = delete;

    /// 创建 Qt 控件（要求 QApplication 已在当前线程创建）。
    bool Initialize(std::string& error);

    /// 释放窗口资源，可重复调用。
    void Shutdown();

    /// 设置气泡文本。空串表示「没有台词」（清空文本，不改变显隐）。
    /// 显隐由 SetVisible() 单独控制，便于「托盘关掉气泡但台词仍在推进」这类场景。
    void SetText(const std::string& text);

    /// 把气泡放到屏幕坐标 (x, y) 附近（通常取桌宠窗口上方的位置）。
    void MoveTo(int x, int y);

    /// 依据渲染窗口大小调整气泡最大宽度（同时等比调整字号）。
    /// 桌宠窗口变宽后，固定 280px 的气泡会显得又小又挤；调用方每帧传入
    /// 按窗口宽度算出的值即可，内部会短路相同值避免重复设置样式表。
    void SetMaxWidth(int pixels);
    int MaxWidth() const { return maxWidth_; }

    /// 气泡窗口当前实际宽高（用于把它摆在渲染窗口正上方、水平居中）。
    int Width() const;
    int Height() const;

    /// 显隐开关（托盘「台词气泡」菜单项控制）。
    void SetVisible(bool visible);
    bool IsVisible() const;

    /// 当前文本（供诊断面板/日志读取）。
    std::string Text() const;

    /// 底层 QWidget 句柄（供测试与调试读取控件树；未创建时返回 nullptr）。
    QWidget* Handle() const { return window_; }

private:
    QWidget* window_ = nullptr;
    QLabel* label_ = nullptr;
    int maxWidth_ = 280;
};

}  // namespace ppet
