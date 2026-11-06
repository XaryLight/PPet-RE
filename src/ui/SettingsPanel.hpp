// PPet - C++ 移植版
// 设置界面：独立的 Qt 6 窗口（QDialog + QTabWidget）。
//
// 【为什么用 Qt 而不是 ImGui】
//   原实现用 Dear ImGui 在一个独立的 GLFW 窗口里绘制界面。ImGui 需要自己的
//   GL 上下文与字体合并，跨平台与中文渲染都要手写，且与主窗口的 GL 上下文
//   容易互相污染。改用 Qt 6 Widgets 后：
//     - 原生窗口、原生控件、系统主题，中文与高 DPI 天然支持；
//     - 不需要第二个 GL 上下文，彻底消除「设置窗口初始化失败 / 污染主窗口渲染」；
//     - 文件选择等交互直接用 QFileDialog，体验更好。
//
// 【与主循环的关系】
//   主窗口（GLFW + OpenGL）依然由 App 的主循环驱动；Qt 不抢占主线程，
//   而是每隔一帧由 App 调用 PumpEvents() 手动驱动 Qt 的事件分发
//   （等价于在主线程里周期性调用 QApplication::processEvents()）。
//   这样 Qt 与 GLFW 共用同一条主线程，既不需要额外的 Qt 线程，也避免了跨线程
//   操作 QWidget 的线程安全问题。
//
// 【实现说明】
//   本类本身不是 QObject，所有信号都用 lambda 连接到控件，因此不需要 moc、
//   不需要 AUTOMOC、不需要在头文件里写 Q_OBJECT，CMake 里也不必为此开 AUTOMOC。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/Config.hpp"
#include "core/I18n.hpp"

class QWidget;

namespace ppet {

/// 设置界面面板（Qt 6 实现）。
///
/// 生命周期：
///   Initialize()  创建 QDialog 与全部控件；
///   Show()/Hide() 显示 / 隐藏窗口（隐藏不销毁，保留界面状态）；
///   PumpEvents()  每帧由 App 调用，驱动 Qt 事件分发；
///   Refresh()     用最新的 strings / config 重绘文案与控件值（语言切换、
///                 清除设置后调用）；
///   Shutdown()    释放对话框资源。
///
/// 数据绑定由上层（App）填充：
///   config            指向要编辑的配置对象，面板直接读写它的字段；
///   strings           界面文案（本地化）；
///   onConfigChanged   配置被修改后回调（上层负责持久化与生效）；
///   onImportModel     导入模型（本地路径）；
///   onRemoveModel     删除模型列表中的第 index 项；
///   onSelectModel     切换到模型列表中的第 index 项；
///   onClearSettings   「清除设置」：把配置恢复为默认值。
class SettingsPanel {
public:
    SettingsPanel();
    ~SettingsPanel();

    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    /// 创建 Qt 对话框与全部控件。
    ///
    /// 要求 QApplication 已经在本线程创建（见 main.cpp）。
    /// @param error 失败时写入中文原因
    /// @return 成功返回 true
    bool Initialize(std::string& error);

    /// 释放对话框资源。可重复调用。
    void Shutdown();

    /// 每帧由 App 调用，驱动 Qt 事件分发（替代原来的 Render()）。
    void PumpEvents();

    /// 窗口当前是否可见。
    bool IsVisible() const;

    /// 显示设置窗口（并置顶）。
    void Show();

    /// 隐藏设置窗口（不销毁，保留界面状态）。
    void Hide();

    /// 可见性取反。
    void Toggle();

    /// 设置窗口的 QWidget 指针（未创建时为 nullptr）。
    QWidget* Handle() const;

    /// 用最新的 strings / config 刷新界面文案与控件值。
    void Refresh();

    /// 弹出「导入模型」文件对话框（托盘菜单与面板内按钮共用）。
    void OpenImportDialog();

    // -------------------------------------------------------------------------
    // 数据绑定（由 App 层填充）
    // -------------------------------------------------------------------------
    AppConfig* config = nullptr;
    Strings* strings = nullptr;
    std::function<void()> onConfigChanged;
    std::function<void(const std::string&)> onImportModel;
    std::function<void(int)> onRemoveModel;
    std::function<void(int)> onSelectModel;
    std::function<void()> onClearSettings;

private:
    /// 构建全部控件与布局（仅在 Initialize 里调用一次）。
    void BuildUi();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ppet
